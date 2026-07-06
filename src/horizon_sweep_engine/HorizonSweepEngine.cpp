#include "HorizonSweepEngine.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PngWrite.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
}  // namespace

HorizonSweepEngine::HorizonSweepEngine(ElevationSource&      dem,
                                       WaterQuery&           water,
                                       const PipelineConfig& config,
                                       double min_lat, double max_lat,
                                       double min_lon, double max_lon)
    : dem_(dem),
      water_(water),
      config_(config),
      min_lat_(min_lat),
      max_lat_(max_lat),
      min_lon_(min_lon),
      max_lon_(max_lon) {
    cell_deg_ = 1.0 / config_.cell_per_degree;
    width_  = static_cast<int>(std::lround((max_lon_ - min_lon_) * config_.cell_per_degree));
    height_ = static_cast<int>(std::lround((max_lat_ - min_lat_) * config_.cell_per_degree));
}

void HorizonSweepEngine::compute_slice(double azimuth_deg, AzimuthSlice& out) {
    // Size the caller-owned buffer once; reuse it across calls (allocation only
    // happens on the first call, when the buffer is empty or wrongly sized).
    const std::size_t pixels = static_cast<std::size_t>(width_) * height_;
    if (out.visible.size() != pixels) {
        out.visible.assign(pixels, false);
    } else {
        std::fill(out.visible.begin(), out.visible.end(), false);
    }
    out.width   = width_;
    out.height  = height_;
    out.min_lat = min_lat_;
    out.min_lon = min_lon_;

    // Ocean->land march bearing (reverse of the sunset azimuth).
    const double bearing = std::fmod(azimuth_deg + 180.0, 360.0);

    // Curvature + refraction drop coefficient: h_adjusted = h - d^2 * c.
    const double k = config_.refraction_coefficient_k;
    const double R = config_.earth_radius_m;
    const double c = (1.0 - 2.0 * k) / (2.0 * R);
    const double offset = config_.horizon_reference_offset_m;
    const double skip   = config_.coast_obstruction_skip_m;  // passable foreshore depth (m)
    const double eye    = config_.observer_eye_height_m;
    const double mpd    = config_.meters_per_degree_lat;

    // Sample spacing s (ADR-0014): the single density knob. Drives BOTH the
    // perp ray spacing and the along march step, so the Visibility Sample Grid
    // is square in the ray frame.
    const double s = (config_.sample_spacing_arcsec / 3600.0) * mpd;
    const double coast_max_m = config_.coast_march_max_km * 1000.0;

    // ── Rotated frame (ADR-0007) ────────────────────────────────────────
    // Anchor a flat-earth local metric at the box's south-west corner. For any
    // pixel (lat, lon):
    //   E = (lon - lon0) * mpd * cos(phi0)   N = (lat - lat0) * mpd
    //   along =  E*sin b + N*cos b           perp =  E*cos b - N*sin b
    // The ray index j = round(perp / s) partitions every pixel into exactly one
    // parallel ray. At b = 90 deg perp depends only on latitude, so each output
    // row is one ray (the cardinal case).
    const double bearing_rad = deg2rad(bearing);
    const double sin_b   = std::sin(bearing_rad);
    const double cos_b   = std::cos(bearing_rad);
    const double cosphi0 = std::cos(deg2rad(min_lat_));
    const double mE      = mpd * cosphi0;   // metres east per degree of longitude
    const double A       = cell_deg_ * mE;  // E increment per output column
    const double Nrow    = cell_deg_ * mpd; // N increment per output row

    // along/perp of a pixel relative to the box anchor.
    auto along_of = [&](double E, double N) { return E * sin_b + N * cos_b; };
    auto perp_of  = [&](double E, double N) { return E * cos_b - N * sin_b; };

    // The march is a STRAIGHT LINE in this frame: one step advances the frame
    // coordinates by exactly (s·sin b, s·cos b), and lat/lon are derived from
    // (E, N) only to query water and elevation. This is what makes the whole
    // slice one consistent geometry (ADR-0014 amendment): the gather assigns a
    // pixel to ray round(perp/s), and because rays never leave their frame
    // lines, that IS the ray whose march passes within s/2 of the pixel.
    // (Stepping longitude by the local cos(lat) instead — a per-step rhumb
    // line — bends the marched path off its frame line by ~200 m over a 70 km
    // offshore run, so pixels inherited verdicts from rays that physically
    // passed hundreds of metres away.)
    const double dE = s * sin_b;
    const double dN = s * cos_b;
    auto lat_of = [&](double N) { return min_lat_ + N / mpd; };
    auto lon_of = [&](double E) { return min_lon_ + E / mE; };

    // ── Diagnostic trace setup (no-op unless trace_.enabled) ────────────────
    // Resolve which parallel ray carries the target: round(perp/s), exactly the
    // gather's own assignment. Because the march is frame-straight, this ray is
    // both the one whose march passes within s/2 of the target AND the one the
    // target pixel inherits its verdict from — the trace shows the very
    // samples the map is built from.
    long trace_j = LONG_MIN;
    if (trace_.enabled) {
        const double Et = (trace_.target_lon - min_lon_) * mE;
        const double Nt = (trace_.target_lat - min_lat_) * mpd;
        trace_j = std::lround(perp_of(Et, Nt) / s);
    }
    struct TraceStep {
        int    idx; double along, d, lat, lon; float h; double h_adj;
        bool   in_skip; double running_max; double obs_slope; bool visible;
    };
    std::vector<TraceStep> trace_rec;

    // Ray index range: perp and along are linear, so the extremes lie at the
    // box corners. along_max bounds how far each ray must march.
    double perp_min = 1e300, perp_max = -1e300, along_max = -1e300;
    for (int r : {0, height_}) {
        for (int col : {0, width_}) {
            const double E = col * A, N = r * Nrow;
            perp_min  = std::min(perp_min, perp_of(E, N));
            perp_max  = std::max(perp_max, perp_of(E, N));
            along_max = std::max(along_max, along_of(E, N));
        }
    }
    const long j_min = std::lround(perp_min / s) - 1;
    const long j_max = std::lround(perp_max / s) + 1;

    // Full-grid debug dump: reset retained rays for this slice (rays that find
    // no coast keep an empty verdict list).
    if (retain_grid_) {
        grid_.j_min = j_min;
        grid_.j_max = j_max;
        grid_.rays.assign(static_cast<std::size_t>(j_max - j_min + 1), GridRay{});
    }

    // Pre-size verdicts_ once for the longest possible ray in this slice, then
    // write by index in the march (see below). After the first call this resize
    // is a no-op (capacity is sticky and the size matches), so the march emits
    // straight indexed stores.
    const std::size_t max_verdicts_n =
        static_cast<std::size_t>(along_max / s) + 16;
    if (verdicts_.size() < max_verdicts_n) verdicts_.resize(max_verdicts_n);

    // |A*cos b| is the per-column change in perp; when it is ~0 the azimuth is
    // cardinal and a whole output row maps to one ray (perp is column-independent).
    const double denom = A * cos_b;
    constexpr double kCardinalEps = 1e-6;

    for (long j = j_min; j <= j_max; ++j) {
        // Seed this ray at its intersection with the western edge (E = 0, open
        // ocean): perp = -N*sin b = j*s, so N = -j*s/sin b. sin b is bounded
        // away from 0 over the sunset range (the cardinal-east case at azimuth
        // 270 has sin b = 1), so this never divides by ~0.
        double E = 0.0;
        double N = -static_cast<double>(j) * s / sin_b;

        // ── Coast search: the front of the single march ─────────────────
        // Step along the bearing at s, querying is_water each sample, until it
        // flips to land: that sample is the coastline crossing. A ray with no
        // coast within the give-up distance yields no visible samples — its
        // pixels stay not-visible.
        bool coast_found = false;
        for (double marched = 0.0; marched <= coast_max_m; marched += s) {
            if (!water_.is_water(lat_of(N), lon_of(E))) { coast_found = true; break; }
            E += dE;
            N += dN;
        }
        if (!coast_found) continue;

        const double Ec = E, Nc = N;   // crossing, in frame coordinates
        const double coast_lat = lat_of(Nc);
        const double coast_lon = lon_of(Ec);
        const double along_c   = along_of(Ec, Nc);

        // When along_c < 0 the march starts before the box origin (coast
        // crossing north of the box for diagonal bearings with cos_b < 0). The
        // pre-allocation above only budgets for along_c = 0; resize only when
        // necessary — after the worst-case ray this guard becomes a no-op.
        if (along_c < 0.0) {
            const std::size_t needed =
                static_cast<std::size_t>((along_max + s - along_c) / s) + 16;
            if (verdicts_.size() < needed) verdicts_.resize(needed);
        }

        // ── Verdict march (the new core, ADR-0014) ──────────────────────
        // Continue the same march inland from the crossing, accumulating d
        // (from the Horizon Reference offset) and the running maximum slope
        // (bare ground only), and store a FINISHED visible/not verdict at
        // every sample: visible = (h_adj + eye)/d >= running_max. The first
        // land sample is visible emergently (h ~ 0, d = offset,
        // running_max = 0). Terminate by along-ray distance so the ray spans
        // every in-box pixel regardless of where the tilted ray enters or
        // leaves the latitude band.
        char* const verd = verdicts_.data();
        int n = 0;
        double d     = offset;
        double along = along_c;
        double running_max = 0.0;
        const bool tracing = trace_.enabled && j == trace_j;
        while (along <= along_max + s) {
            const double lat = lat_of(N);
            const double lon = lon_of(E);
            float h = dem_.get_elevation(lat, lon);
            if (std::isnan(h)) h = 0.0f;  // no data: treat as ocean surface
            const double h_adj = h - d * d * c;
            // Terrain within the passable foreshore (first `skip` metres inland
            // of the crossing) does not raise the obstruction profile: low
            // foredunes and berms right at the shore should not shadow the flat
            // in their own lee where an observer plainly sees the ocean
            // horizon. Real relief farther inland still accumulates normally.
            const bool in_skip = (d - offset < skip);
            if (!in_skip)
                running_max = std::max(running_max, h_adj / d);
            const double obs_slope = (h_adj + eye) / d;
            const bool   visible   = obs_slope >= running_max;
            verd[n++] = visible ? 1 : 0;
            if (tracing && n - 1 <= trace_.steps_after)
                trace_rec.push_back({n - 1, along, d, lat, lon, h, h_adj,
                                     in_skip, running_max, obs_slope, visible});

            E     += dE;
            N     += dN;
            d     += s;
            along += s;
        }
        if (n == 0) continue;
        const int last = n - 1;

        if (retain_grid_) {
            GridRay& ray = grid_.rays[static_cast<std::size_t>(j - j_min)];
            ray.along_coast = along_c;
            ray.coast_lat   = coast_lat;
            ray.coast_lon   = coast_lon;
            ray.verdicts.assign(verd, verd + n);
        }

        // ── Diagnostic trace dump (target ray only) ─────────────────────
        if (tracing) {
            if (trace_.on_resolved)
                trace_.on_resolved(trace_j, coast_lat, coast_lon);
            std::printf(
                "\n==== HorizonSweepEngine ray trace ====\n"
                "sunset azimuth   : %.3f deg\n"
                "march bearing    : %.3f deg (ocean->land)\n"
                "target           : (%.6f, %.6f)\n"
                "ray index j      : %ld\n"
                "coast crossing   : (%.6f, %.6f)  [sample 0]\n"
                "along_c          : %.3f m\n"
                "config: offset(d@coast)=%.1f m  eye=%.2f m  skip=%.1f m  "
                "s=%.4f m  k=%.3f  R=%.0f m  c=%.6e\n\n",
                azimuth_deg, bearing, trace_.target_lat, trace_.target_lon,
                trace_j, coast_lat, coast_lon, along_c,
                offset, eye, skip, s, k, R, c);

            std::printf(
                "%6s %12s %10s %10s %11s %11s %9s %8s %5s %12s %12s %4s %4s\n",
                "step", "lat", "lon", "d_m", "along_m", "elev_m", "h_adj",
                "water", "skip", "max_slope", "obs_slope", "vis", "side");
            std::printf(
                "------ ------------ ---------- ---------- ----------- "
                "----------- --------- -------- ----- ------------ "
                "------------ ---- ----\n");

            auto water_at = [&](double la, double lo) -> const char* {
                if (!trace_.water_query) return "?";
                return trace_.water_query(la, lo) ? "water" : "land";
            };

            // Seaward context: step back from the crossing along the ray's
            // frame line (positions are closed-form — the ray is straight).
            // These samples are seaward of the crossing, so the gather always
            // leaves their pixels not-visible (dist < 0); we print is_water and
            // elevation so the coastline crossing itself is visible in the table.
            const int nb = trace_.steps_before;
            for (int m = nb; m >= 1; --m) {
                const double la = lat_of(Nc - m * dN);
                const double lo = lon_of(Ec - m * dE);
                const double dd = offset - m * s;
                float h = dem_.get_elevation(la, lo);
                char elev[16];
                if (std::isnan(h))
                    std::snprintf(elev, sizeof elev, "%11s", "nodata");
                else
                    std::snprintf(elev, sizeof elev, "%11.3f", (double)h);
                std::printf(
                    "%6d %12.6f %10.6f %10.2f %11.2f %s %9s %8s %5s %12s "
                    "%12s %4s %4s\n",
                    -m, la, lo, dd, -static_cast<double>(m) * s, elev,
                    "-", water_at(la, lo), "-", "-", "-", "F", "sea");
            }

            // Inland: replay the recorded march samples. `vis` is the STORED
            // verdict — the exact bool output pixels inherit via the gather.
            for (const TraceStep& t : trace_rec) {
                std::printf(
                    "%6d %12.6f %10.6f %10.2f %11.2f %11.3f %9.3f %8s %5s "
                    "%12.7f %12.7f %4s %4s\n",
                    t.idx, t.lat, t.lon, t.d, t.along, (double)t.h, t.h_adj,
                    water_at(t.lat, t.lon), t.in_skip ? "yes" : "no",
                    t.running_max, t.obs_slope, t.visible ? "T" : "F", "land");
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        // ── Inverse/gather to output (ADR-0014) ─────────────────────────
        // For every pixel on this ray (a contiguous column interval per row,
        // found by inverting perp), inherit the nearest sample's stored
        // verdict. No per-pixel physics re-evaluation; water/no-data pixels
        // are left for the downstream water mask (ADR-0013).
        for (int row = 0; row < height_; ++row) {
            const double N = row * Nrow;

            int col_lo, col_hi;
            if (std::fabs(denom) < kCardinalEps) {
                // Cardinal: perp is constant across the row -> all or nothing.
                if (std::lround(perp_of(0.0, N) / s) != j) continue;
                col_lo = 0;
                col_hi = width_ - 1;
            } else {
                // round(perp/s) == j  <=>  perp in [(j-0.5)s, (j+0.5)s).
                const double lo = ((j - 0.5) * s + N * sin_b) / denom;
                const double hi = ((j + 0.5) * s + N * sin_b) / denom;
                double x1 = lo, x2 = hi;
                if (x1 > x2) std::swap(x1, x2);
                // Pad by one column; the exact membership re-check below keeps
                // the partition disjoint, so a slightly wide interval is safe.
                col_lo = static_cast<int>(std::floor(x1)) - 1;
                col_hi = static_cast<int>(std::ceil(x2)) + 1;
                if (col_lo < 0) col_lo = 0;
                if (col_hi > width_ - 1) col_hi = width_ - 1;
                if (col_lo > col_hi) continue;
            }

            for (int col = col_lo; col <= col_hi; ++col) {
                const double E = col * A;
                if (std::lround(perp_of(E, N) / s) != j) continue;  // not this ray
                const double dist = along_of(E, N) - along_c;
                // Nearest-neighbour on the seaward end too. The coast search
                // steps in whole increments of s and stops at the FIRST land
                // sample, so along_c is biased up to a full sample INLAND of the
                // true water/land boundary (which lies between the last-water
                // and first-land samples). A land pixel can therefore sit up to
                // a full s seaward of along_c and still be genuine beach; snap it
                // to sample 0 rather than drop it. Only pixels beyond that full
                // step are truly seaward and stay false; if they are ocean the
                // downstream water mask (ADR-0013) repaints them regardless, so
                // erring generous here is safe.
                if (dist < -s) continue;

                int idx = static_cast<int>(std::lround(dist / s));
                if (idx < 0) idx = 0;
                if (idx > last) idx = last;
                out.visible[static_cast<std::size_t>(row) * width_ + col] =
                    verd[idx] != 0;
            }
        }
    }

    if (retain_grid_ && !grid_png_path_.empty())
        write_sample_grid_png(grid_, grid_png_path_);
}

bool write_sample_grid_png(const HorizonSweepEngine::SampleGrid& grid,
                           const std::string& path) {
    if (grid.j_max < grid.j_min || grid.rays.empty()) return false;

    std::size_t max_n = 0;
    for (const auto& ray : grid.rays)
        max_n = std::max(max_n, ray.verdicts.size());
    if (max_n == 0) return false;

    const int width  = static_cast<int>(max_n);
    const int height = static_cast<int>(grid.rays.size());
    std::vector<uint8_t> img(static_cast<std::size_t>(width) * height, 0);
    for (int r = 0; r < height; ++r) {
        const auto& v = grid.rays[static_cast<std::size_t>(r)].verdicts;
        uint8_t* row = &img[static_cast<std::size_t>(r) * width];
        for (std::size_t kk = 0; kk < v.size(); ++kk)
            row[kk] = v[kk] ? 255 : 96;
    }
    return write_gray_png(path, width, height, img);
}
