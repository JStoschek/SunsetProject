// Analytic per-sample verdict tests for the forward-sampled Visibility Sample
// Grid (ADR-0014). Each scenario builds a synthetic DEM via the injected
// ElevationSource, runs compute_slice at the cardinal sunset azimuth (270°,
// bearing due east — the march path is closed-form), and asserts EXACT
// verdicts on a single ray read back through the retained grid
// (set_grid_dump + sample_grid()).
//
// Expected verdicts come from scenario-specific CLOSED FORMS (curvature
// horizon d = sqrt(eye/c), obstruction slope comparisons), not from
// re-running the engine's recurrence. Every feature edge is placed half a
// sample spacing away from the march lattice so sample->terrain assignment is
// unambiguous, and every asserted sample carries a wide physical margin.
//
// Scenarios (PRD engine-forward-sampled-grid §8.3):
//   1. flat ocean -> flat land: visible exactly out to the curvature horizon;
//   2. single tall ridge: lee samples blocked (and the crest sees over itself);
//   3. two ridges (near shorter, far taller): visible, blocked, visible AGAIN
//      — a disjoint Visible Azimuth Set along one ray;
//   4. a low berm below eye height does not block the flat behind it (and the
//      same berm above eye height does);
//   5. curvature: the same low ridge blocks its lee when near, but placed far
//      enough out that d²c swallows it (h_adj < 0) it stops blocking —
//      the d²c term, closed form.
//
// The engines here run with coast_obstruction_skip_m = 0 so the obstruction
// math is exercised undamped; the foreshore skip is a separate tunable.
//
// PIPELINE_CONF_PATH is injected by CMake so the tests stay config-driven.
#include "Fakes.h"
#include "PipelineConfig.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }

// Hard check that survives NDEBUG (Release builds compile assert() out).
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
            return 1;                                                 \
        }                                                             \
    } while (0)

constexpr double kSunsetWest = 270.0;  // bearing 90°: the march runs due east
constexpr double kCoastLon   = -122.95;

// Single-row box: one output row, so ray j = 0 carries it. 0.15° of longitude
// gives ~9 km of land beyond the coast — room for the far-ridge scenarios.
struct Box {
    double min_lat = 37.0;
    double max_lat = 37.0 + 1.0 / 10800.0;  // exactly one row
    double min_lon = -123.0;
    double max_lon = -122.85;
};

// Everything analytic about one cardinal ray, derived from the config.
struct RayMath {
    double s;        ///< sample spacing (m)
    double dlon;     ///< march longitude step (deg) — constant at bearing 90
    double c;        ///< curvature coefficient (1-2k)/2R
    double offset;   ///< Horizon Reference offset (d at the coast crossing)
    double eye;
    long   i_coast;  ///< march index (from the western seed) of the crossing

    explicit RayMath(const PipelineConfig& cfg, const Box& box) {
        s      = (cfg.sample_spacing_arcsec / 3600.0) * cfg.meters_per_degree_lat;
        dlon   = s / (cfg.meters_per_degree_lat * std::cos(deg2rad(box.min_lat)));
        c      = (1.0 - 2.0 * cfg.refraction_coefficient_k)
                 / (2.0 * cfg.earth_radius_m);
        offset = cfg.horizon_reference_offset_m;
        eye    = cfg.observer_eye_height_m;
        // The engine seeds ray 0 at (min_lat, min_lon) and steps dlon east per
        // sample; the crossing is the first sample at or east of the coast.
        i_coast = static_cast<long>(
            std::ceil((kCoastLon - box.min_lon) / dlon - 1e-9));
    }

    double d_of(std::size_t k) const { return offset + static_cast<double>(k) * s; }

    // Longitude bounds of the terrain feature occupying ray samples
    // [k_lo, k_hi] (indexed from the crossing), edges half a lattice step out.
    double lon_lo(long k_lo, const Box& box) const {
        return box.min_lon + (static_cast<double>(i_coast + k_lo) - 0.5) * dlon;
    }
    double lon_hi(long k_hi, const Box& box) const {
        return box.min_lon + (static_cast<double>(i_coast + k_hi) + 0.5) * dlon;
    }

    // Obstruction slope of a feature of height h whose FIRST sample is k
    // (the running maximum a plateau contributes — later samples of the same
    // height have strictly smaller bare slope).
    double feature_slope(double h, long k) const {
        const double d = d_of(static_cast<std::size_t>(k));
        return (h - d * d * c) / d;
    }
};

// Run one slice with grid retention and hand back ray 0's verdicts.
const HorizonSweepEngine::GridRay& run_ray(HorizonSweepEngine& engine,
                                           AzimuthSlice&       slice) {
    engine.set_grid_dump(true);
    engine.compute_slice(kSunsetWest, slice);
    const auto& grid = engine.sample_grid();
    return grid.rays[static_cast<std::size_t>(0 - grid.j_min)];
}

}  // namespace

int main() {
    PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);
    config.coast_obstruction_skip_m = 0.0;  // exercise the undamped obstruction math
    const Box box;
    const RayMath m(config, box);
    FakeWater water(kCoastLon);

    CHECK(m.c > 0.0, "setup: curvature coefficient must be positive (k < 0.5)");
    CHECK(m.eye > 0.0, "setup: eye height must be positive");
    const double d_horizon = std::sqrt(m.eye / m.c);  // flat-land curvature horizon
    CHECK(d_horizon > m.offset + 50.0 * m.s,
          "setup: the horizon must lie many samples past the coast");

    // ── 1. Flat ocean -> flat land: visible exactly to the curvature horizon ─
    // Closed form: running_max stays 0 on 0 m terrain, so sample k is visible
    // iff eye >= d_k² c, i.e. d_k <= sqrt(eye/c). Samples whose margin is
    // within 0.1 mm of the boundary are skipped (config could in principle
    // land the boundary on a lattice point).
    {
        FakeDEM dem([](double, double) { return 0.0f; });
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        const auto& ray = run_ray(engine, slice);
        CHECK(!ray.verdicts.empty(), "flat: ray must have samples");
        CHECK(ray.coast_lon >= kCoastLon &&
                  ray.coast_lon < kCoastLon + 2.0 * m.dlon,
              "flat: crossing must sit within one sample of the coast meridian");

        int flips = 0;
        for (std::size_t k = 0; k < ray.verdicts.size(); ++k) {
            const double d      = m.d_of(k);
            const double margin = m.eye - d * d * m.c;
            if (std::fabs(margin) < 1e-4) continue;  // boundary sample: skip
            const bool expected = margin > 0.0;
            CHECK(ray.verdicts[k] == (expected ? 1 : 0),
                  "flat: every sample matches the closed-form curvature horizon");
            if (k > 0 && ray.verdicts[k] != ray.verdicts[k - 1]) ++flips;
        }
        CHECK(ray.verdicts[0] == 1,
              "flat: the first land sample is visible emergently");
        CHECK(flips == 1, "flat: exactly one visible->blocked transition");
        std::puts("PASS: flat land visible exactly to the closed-form curvature horizon");
    }

    // ── 2. Single tall ridge: lee samples blocked; the crest sees over itself ─
    {
        const long   k1 = std::lround(1500.0 / m.s);  // ridge front ~1.5 km inland
        const long   k2 = k1 + 9;                     // ~10 samples wide
        const double H  = 500.0;
        const double lo = m.lon_lo(k1, box), hi = m.lon_hi(k2, box);
        FakeDEM dem([lo, hi, H](double, double lon) {
            return (lon >= lo && lon <= hi) ? static_cast<float>(H) : 0.0f;
        });
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        const auto& ray = run_ray(engine, slice);

        // Setup: the ridge towers over anything eye height can reach at any
        // in-box distance, so every lee sample must be blocked.
        const double ridge_slope = m.feature_slope(H, k1);
        CHECK(ridge_slope > m.eye / m.offset,
              "setup: ridge slope exceeds the steepest possible flat-lee slope");

        for (long k = 0; k < k1; ++k) {
            const double d      = m.d_of(static_cast<std::size_t>(k));
            const double margin = m.eye - d * d * m.c;
            CHECK(margin > 1e-4, "setup: pre-ridge samples are inside the horizon");
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 1,
                  "single ridge: samples before the ridge are visible");
        }
        CHECK(ray.verdicts[static_cast<std::size_t>(k1)] == 1,
              "single ridge: the crest sees over itself (eye added at the check)");
        for (std::size_t k = static_cast<std::size_t>(k2 + 1);
             k < ray.verdicts.size(); ++k) {
            CHECK(ray.verdicts[k] == 0,
                  "single ridge: every lee sample is blocked");
        }
        std::puts("PASS: a single tall ridge blocks every lee sample");
    }

    // ── 3. Two ridges: visible, blocked, visible AGAIN (disjoint bands) ──────
    // A short ridge ~2 km inland shadows the flat behind it; a steep tall ramp
    // ~6 km inland rises fast enough that its own slope overtakes the near
    // ridge's — the ray's verdict list goes 1...1 0...0 1...1.
    {
        const long   ka1 = std::lround(2000.0 / m.s), ka2 = ka1 + 4;  // near ridge
        const double H1  = 50.0;
        const long   kb1 = std::lround(6000.0 / m.s);                 // ramp start
        const long   kb2 = kb1 + std::lround(500.0 / m.s);            // ~500 m long
        const double B   = 150.0;   // ramp base height
        const double G   = 1.0;     // ramp grade: +1 m per metre east

        const double a_lo = m.lon_lo(ka1, box), a_hi = m.lon_hi(ka2, box);
        const double b_lo = m.lon_lo(kb1, box), b_hi = m.lon_hi(kb2, box);
        const double mE   = config.meters_per_degree_lat *
                            std::cos(deg2rad(box.min_lat));
        FakeDEM dem([=](double, double lon) -> float {
            if (lon >= a_lo && lon <= a_hi) return static_cast<float>(H1);
            if (lon >= b_lo && lon <= b_hi)
                return static_cast<float>(B + G * (lon - b_lo) * mE);
            return 0.0f;
        });
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        const auto& ray = run_ray(engine, slice);

        const double near_slope = m.feature_slope(H1, ka1);
        // Setup: the gap between the ridges is fully shadowed by the near
        // ridge (its slope beats eye height everywhere in the gap)...
        for (long k = ka2 + 1; k < kb1; ++k) {
            const double d = m.d_of(static_cast<std::size_t>(k));
            CHECK((m.eye - d * d * m.c) / d < near_slope - 1e-6,
                  "setup: the whole gap lies in the near ridge's shadow");
        }
        // ...and the ramp base already clears the near ridge with margin.
        CHECK(m.feature_slope(B, kb1) > near_slope + 1e-5,
              "setup: the ramp's bare slope alone clears the near ridge");

        for (long k = 0; k < ka1; ++k)
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 1,
                  "two ridges: samples before the near ridge are visible");
        for (long k = ka2 + 1; k < kb1; ++k)
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 0,
                  "two ridges: the gap between the ridges is blocked");
        for (long k = kb1; k <= kb2; ++k)
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 1,
                  "two ridges: the far ramp is visible AGAIN (disjoint bands)");
        std::puts("PASS: two ridges yield visible, blocked, visible again");
    }

    // ── 4. A low berm below eye height does not block the flat behind it ────
    // Berm at 0.75x eye height right behind the coast: an observer close
    // behind it still sees the horizon over it. The same berm at 1.5x eye
    // height blocks the same band. Closed form: band sample k is visible iff
    // (eye - d_k²c)/d_k >= (h_berm - d_b²c)/d_b (worst case: first berm sample).
    {
        const long k_b1 = 1, k_b2 = 6;                    // berm: samples 1..6
        const long k_lo = k_b2 + 2;                       // probe band start
        const long k_hi = std::lround(400.0 / m.s);       // ...to ~400 m inland
        CHECK(k_hi > k_lo + 5, "setup: probe band spans several samples");

        const double lo = m.lon_lo(k_b1, box), hi = m.lon_hi(k_b2, box);
        auto run_with_berm = [&](double berm_h,
                                 AzimuthSlice& slice) -> std::vector<char> {
            FakeDEM dem([lo, hi, berm_h](double, double lon) {
                return (lon >= lo && lon <= hi) ? static_cast<float>(berm_h) : 0.0f;
            });
            HorizonSweepEngine engine(dem, water, config,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            return run_ray(engine, slice).verdicts;
        };

        const double low_h  = 0.75 * m.eye;
        const double tall_h = 1.50 * m.eye;
        const double low_slope  = m.feature_slope(low_h, k_b1);
        const double tall_slope = m.feature_slope(tall_h, k_b1);
        for (long k = k_lo; k <= k_hi; ++k) {
            const double d = m.d_of(static_cast<std::size_t>(k));
            const double band_slope = (m.eye - d * d * m.c) / d;
            CHECK(band_slope > low_slope * 1.05,
                  "setup: the band clears the low berm with margin");
            CHECK(band_slope < tall_slope * 0.95,
                  "setup: the band is shadowed by the tall berm with margin");
        }

        AzimuthSlice slice;
        const std::vector<char> low  = run_with_berm(low_h, slice);
        const std::vector<char> tall = run_with_berm(tall_h, slice);
        for (long k = k_lo; k <= k_hi; ++k) {
            CHECK(low[static_cast<std::size_t>(k)] == 1,
                  "low berm: a berm below eye height does not block the flat behind it");
            CHECK(tall[static_cast<std::size_t>(k)] == 0,
                  "tall berm: the same berm above eye height blocks that band");
        }
        std::puts("PASS: a sub-eye-height berm does not block; a taller one does");
    }

    // ── 5. Curvature: distance under d²c disarms a ridge, closed form ───────
    // The same low ridge is placed near (d²c small: it keeps a positive
    // adjusted slope and shadows a probe band) and far (d²c exceeds its
    // height: h_adj < 0, running_max stays 0, the same-position probe band
    // stays visible). Only the d²c term distinguishes the two runs.
    {
        const double H = 0.75 * m.eye;  // low ridge; sub-horizon everywhere near

        // Far placement: just past 90% of the horizon — d²c > H there.
        const long k_far1 = std::lround((0.90 * d_horizon - m.offset) / m.s);
        const long k_far2 = k_far1 + 4;
        // Near placement: a few samples in — d²c tiny, ridge slope positive.
        const long k_near1 = 10, k_near2 = 14;
        // Probe band: behind the far ridge but still inside the horizon.
        const long k_p1 = k_far2 + 2;
        const long k_p2 = std::lround((0.97 * d_horizon - m.offset) / m.s);
        CHECK(k_p2 > k_p1 + 3, "setup: probe band spans several samples");

        const double d_far = m.d_of(static_cast<std::size_t>(k_far1));
        CHECK(H < d_far * d_far * m.c * 0.999,
              "setup: at the far placement d²c exceeds the ridge height");
        const double near_slope = m.feature_slope(H, k_near1);
        CHECK(near_slope > 0.0, "setup: the near ridge keeps a positive slope");
        for (long k = k_p1; k <= k_p2; ++k) {
            const double d = m.d_of(static_cast<std::size_t>(k));
            const double probe_slope = (m.eye - d * d * m.c) / d;
            CHECK(probe_slope > 1e-7,
                  "setup: the probe band is inside the bare curvature horizon");
            CHECK(probe_slope < near_slope * 0.95,
                  "setup: the near ridge shadows the probe band with margin");
        }

        auto run_with_ridge = [&](long ka, long kb,
                                  AzimuthSlice& slice) -> std::vector<char> {
            const double lo = m.lon_lo(ka, box), hi = m.lon_hi(kb, box);
            FakeDEM dem([lo, hi, H](double, double lon) {
                return (lon >= lo && lon <= hi) ? static_cast<float>(H) : 0.0f;
            });
            HorizonSweepEngine engine(dem, water, config,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            return run_ray(engine, slice).verdicts;
        };

        AzimuthSlice slice;
        const std::vector<char> with_near = run_with_ridge(k_near1, k_near2, slice);
        const std::vector<char> with_far  = run_with_ridge(k_far1, k_far2, slice);
        for (long k = k_p1; k <= k_p2; ++k) {
            CHECK(with_near[static_cast<std::size_t>(k)] == 0,
                  "curvature: the NEAR low ridge shadows the probe band");
            CHECK(with_far[static_cast<std::size_t>(k)] == 1,
                  "curvature: moved out to where d²c swallows it, the SAME ridge "
                  "no longer blocks");
        }
        std::puts("PASS: d²c disarms a distant low ridge (closed-form curvature)");
    }

    // ── Grid dump artifact: the PNG debug switch writes a real PNG ───────────
    {
        FakeDEM dem([](double, double) { return 0.0f; });
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        const char* path = "sample_grid_dump_test.png";
        engine.set_grid_dump(true, path);
        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        FILE* f = std::fopen(path, "rb");
        CHECK(f != nullptr, "grid dump: PNG file must exist");
        unsigned char sig[8] = {0};
        const bool got8 = std::fread(sig, 1, 8, f) == 8;
        std::fclose(f);
        std::remove(path);
        CHECK(got8 && sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' &&
                  sig[3] == 'G',
              "grid dump: file must carry the PNG signature");
        std::puts("PASS: the full-grid debug dump writes a PNG");
    }

    std::puts("ALL PASS");
    return 0;
}
