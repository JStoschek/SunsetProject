#include "HorizonSweepEngine.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
}  // namespace

HorizonSweepEngine::HorizonSweepEngine(ElevationSource&      dem,
                                       CoastlineFinder&      ocean,
                                       const PipelineConfig& config,
                                       double min_lat, double max_lat,
                                       double min_lon, double max_lon)
    : dem_(dem),
      ocean_(ocean),
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
    const double eye    = config_.observer_eye_height_m;
    const double mpd    = config_.meters_per_degree_lat;
    const double step   = config_.march_step_m;

    // ── Rotated frame (ADR-0007) ────────────────────────────────────────
    // Anchor a flat-earth local metric at the box's south-west corner. For any
    // pixel (lat, lon):
    //   E = (lon - lon0) * mpd * cos(phi0)   N = (lat - lat0) * mpd
    //   along =  E*sin b + N*cos b           perp =  E*cos b - N*sin b
    // The ray index j = round(perp / s) partitions every pixel into exactly one
    // parallel ray (s = one cell of latitude in metres). At b = 90 deg perp
    // depends only on latitude, so each output row is one ray (the cardinal case).
    const double bearing_rad = deg2rad(bearing);
    const double sin_b   = std::sin(bearing_rad);
    const double cos_b   = std::cos(bearing_rad);
    const double cosphi0 = std::cos(deg2rad(min_lat_));
    const double mE      = mpd * cosphi0;   // metres east per degree of longitude
    const double A       = cell_deg_ * mE;  // E increment per output column
    const double Nrow    = cell_deg_ * mpd; // N increment per output row
    const double s       = cell_deg_ * mpd; // one cell of latitude in metres

    // along/perp of a pixel relative to the box anchor.
    auto along_of = [&](double E, double N) { return E * sin_b + N * cos_b; };
    auto perp_of  = [&](double E, double N) { return E * cos_b - N * sin_b; };

    // Ray index range: perp and along are linear, so the extremes lie at the
    // box corners. along_max bounds how far Phase 1 must march.
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

    // |A*cos b| is the per-column change in perp; when it is ~0 the azimuth is
    // cardinal and a whole output row maps to one ray (perp is column-independent).
    const double denom = A * cos_b;
    constexpr double kCardinalEps = 1e-6;

    for (long j = j_min; j <= j_max; ++j) {
        // Seed this ray at its intersection with the western edge (E = 0, open
        // ocean): perp = -N*sin b = j*s, so N = -j*s/sin b. sin b is bounded
        // away from 0 over the 255-285 deg sunset range, so this never divides
        // by ~0. The coastline finder marches along the bearing to the crossing.
        const double seed_N   = -static_cast<double>(j) * s / sin_b;
        const double seed_lat = min_lat_ + seed_N / mpd;
        const OceanOriginResult cross =
            ocean_.ocean_origin_for_ray(bearing, seed_lat, min_lon_);

        const double Ec = (cross.coast_lon - min_lon_) * mE;
        const double Nc = (cross.coast_lat - min_lat_) * mpd;
        const double along_c = along_of(Ec, Nc);

        // ── Phase 1: obstruction profile ────────────────────────────────
        // March from the crossing along the bearing, accumulating d (from the
        // Horizon Reference offset) and storing running_max_slope inclusive of
        // each step (bare ground only). Terminate by along-ray distance so the
        // profile spans every in-box pixel on this ray regardless of where the
        // tilted ray enters or leaves the latitude band.
        profile_.clear();
        double lat = cross.coast_lat;
        double lon = cross.coast_lon;
        double d   = offset;
        double along = along_c;
        double running_max = 0.0;
        while (along <= along_max + step) {
            float h = dem_.get_elevation(lat, lon);
            if (std::isnan(h)) h = 0.0f;
            const double h_adj = h - d * d * c;
            running_max = std::max(running_max, h_adj / d);
            profile_.push_back(static_cast<float>(running_max));

            const double dnorth_m = step * cos_b;
            const double deast_m  = step * sin_b;
            lat += dnorth_m / mpd;
            lon += deast_m / (mpd * std::cos(deg2rad(lat)));
            d     += step;
            along += step;
        }
        if (profile_.empty()) continue;
        const int last = static_cast<int>(profile_.size()) - 1;

        // ── Phase 2: observers ──────────────────────────────────────────
        // For every pixel on this ray (a contiguous column interval per row,
        // found by inverting perp), read running_max_slope at the pixel's
        // along-ray step and mark it visible iff (h_adj + eye)/d >= that slope.
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

            const double lat_pix = min_lat_ + row * cell_deg_;
            for (int col = col_lo; col <= col_hi; ++col) {
                const double E = col * A;
                if (std::lround(perp_of(E, N) / s) != j) continue;  // not this ray
                const double dist = along_of(E, N) - along_c;
                if (dist < 0.0) continue;  // seaward of the crossing: stays false

                int idx = static_cast<int>(std::lround(dist / step));
                if (idx < 0) idx = 0;
                if (idx > last) idx = last;
                const double rms = profile_[idx];

                const double lon_pix = min_lon_ + col * cell_deg_;
                float h = dem_.get_elevation(lat_pix, lon_pix);
                if (std::isnan(h)) h = 0.0f;
                const double d_pix = offset + dist;
                const double h_adj = h - d_pix * d_pix * c;
                const bool visible = (h_adj + eye) / d_pix >= rms;
                out.visible[static_cast<std::size_t>(row) * width_ + col] = visible;
            }
        }
    }
}
