// Analytic per-sample verdict tests for the forward-sampled Visibility Sample
// Grid (ADR-0014). Each scenario builds a synthetic DEM via the injected
// ElevationSource, runs compute_slice at the cardinal sunset azimuth (270°,
// bearing due east — the march path is closed-form), and asserts EXACT
// verdicts on a single ray read back through the retained grid
// (set_grid_dump + sample_grid()).
//
// Expected verdicts come from scenario-specific CLOSED FORMS (curvature
// horizon x = sqrt(eye/c) from the crossing, Horizon Reach comparisons —
// ADR-0016), not from re-running the engine's recurrence. Every feature edge
// is placed half a sample spacing away from the march lattice so
// sample->terrain assignment is unambiguous, and every asserted sample
// carries a wide physical margin.
//
// Scenarios (PRD engine-forward-sampled-grid §8.3, re-anchored per ADR-0016):
//   1. flat ocean -> flat land: visible exactly out to the curvature horizon;
//   2. single tall ridge: lee samples blocked (and the crest sees over itself);
//   3. two ridges (near shorter, far taller): visible, blocked, visible AGAIN
//      — a disjoint Visible Azimuth Set along one ray;
//   4. a low berm below eye height does not block the flat behind it (and the
//      same berm above eye height does);
//   5. curvature: the same low ridge blocks its lee when near, but placed far
//      enough out that x²c swallows it (reach < 0) it stops blocking —
//      the curvature term, closed form;
//   6. Long Ridge regression (ADR-0016): a HIGH observer far inland sees the
//      ocean horizon OVER a low coastal hill that hides the near-shore water
//      — the secant model wrongly blocked this class of observer.
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

// Single-row box: one output row, so ray j = 0 carries it. 0.25° of longitude
// gives ~22 km of land beyond the coast — room for the far-inland scenarios.
struct Box {
    double min_lat = 37.0;
    double max_lat = 37.0 + 1.0 / 10800.0;  // exactly one row
    double min_lon = -123.0;
    double max_lon = -122.70;
};

// Everything analytic about one cardinal ray, derived from the config.
struct RayMath {
    double s;        ///< sample spacing (m)
    double dlon;     ///< march longitude step (deg) — constant at bearing 90
    double c;        ///< curvature coefficient (1-2k)/2R
    double eye;
    long   i_coast;  ///< march index (from the western seed) of the crossing

    explicit RayMath(const PipelineConfig& cfg, const Box& box) {
        s      = (cfg.sample_spacing_arcsec / 3600.0) * cfg.meters_per_degree_lat;
        dlon   = s / (cfg.meters_per_degree_lat * std::cos(deg2rad(box.min_lat)));
        c      = (1.0 - 2.0 * cfg.refraction_coefficient_k)
                 / (2.0 * cfg.earth_radius_m);
        eye    = cfg.observer_eye_height_m;
        // The engine seeds ray 0 at (min_lat, min_lon) and steps dlon east per
        // sample; the crossing is the first sample at or east of the coast.
        i_coast = static_cast<long>(
            std::ceil((kCoastLon - box.min_lon) / dlon - 1e-9));
    }

    /// Along-ray distance of sample k inland from the coastline crossing.
    double x_of(std::size_t k) const { return static_cast<double>(k) * s; }

    // Longitude bounds of the terrain feature occupying ray samples
    // [k_lo, k_hi] (indexed from the crossing), edges half a lattice step out.
    double lon_lo(long k_lo, const Box& box) const {
        return box.min_lon + (static_cast<double>(i_coast + k_lo) - 0.5) * dlon;
    }
    double lon_hi(long k_hi, const Box& box) const {
        return box.min_lon + (static_cast<double>(i_coast + k_hi) + 0.5) * dlon;
    }

    // Horizon Reach (ADR-0016) of a feature of height h whose FIRST sample is
    // k: reach = sqrt(h/c) − x, how far seaward of the crossing its own
    // horizon extends. The first sample of a plateau is the running maximum
    // it contributes — later samples of the same height reach strictly less.
    double feature_reach(double h, long k) const {
        return std::sqrt(h / c) - x_of(static_cast<std::size_t>(k));
    }

    // An observer's Horizon Reach at sample k on ground of height h (the eye
    // height is added at the check, ADR-0016).
    double observer_reach(double h, long k) const {
        return std::sqrt((h + eye) / c) - x_of(static_cast<std::size_t>(k));
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
    const double x_horizon = std::sqrt(m.eye / m.c);  // flat-land curvature horizon
    CHECK(x_horizon > 50.0 * m.s,
          "setup: the horizon must lie many samples past the coast");

    // ── 1. Flat ocean -> flat land: visible exactly to the curvature horizon ─
    // Closed form: running_max_reach stays 0 on 0 m terrain (the crossing's
    // sea-surface baseline), so sample k is visible iff eye >= x_k² c, i.e.
    // x_k <= sqrt(eye/c). Samples whose margin is within 0.1 mm of the
    // boundary are skipped (config could in principle land the boundary on a
    // lattice point).
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
            const double x      = m.x_of(k);
            const double margin = m.eye - x * x * m.c;
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

        // Setup: the ridge out-reaches anything eye height can reach on the
        // flat at any in-box distance, so every lee sample must be blocked.
        const double ridge_reach = m.feature_reach(H, k1);
        CHECK(ridge_reach > m.observer_reach(0.0, 0) + 1.0,
              "setup: ridge reach exceeds the best possible flat-lee reach");

        for (long k = 0; k < k1; ++k) {
            const double x      = m.x_of(static_cast<std::size_t>(k));
            const double margin = m.eye - x * x * m.c;
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

        const double near_reach = m.feature_reach(H1, ka1);
        // Setup: the gap between the ridges is fully shadowed by the near
        // ridge (it out-reaches an eye-height observer everywhere in the gap)...
        for (long k = ka2 + 1; k < kb1; ++k) {
            CHECK(m.observer_reach(0.0, k) < near_reach - 1.0,
                  "setup: the whole gap lies in the near ridge's shadow");
        }
        // ...and the ramp base already out-reaches the near ridge with margin.
        CHECK(m.feature_reach(B, kb1) > near_reach + 1.0,
              "setup: the ramp's bare reach alone clears the near ridge");

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
    // sqrt(eye/c) - x_k >= sqrt(h_berm/c) - x_b (worst case: first berm sample).
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
        const double low_reach  = m.feature_reach(low_h, k_b1);
        const double tall_reach = m.feature_reach(tall_h, k_b1);
        for (long k = k_lo; k <= k_hi; ++k) {
            const double band_reach = m.observer_reach(0.0, k);
            CHECK(band_reach > low_reach + 10.0,
                  "setup: the band out-reaches the low berm with margin");
            CHECK(band_reach < tall_reach - 10.0,
                  "setup: the band is out-reached by the tall berm with margin");
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

    // ── 5. Curvature: distance under x²c disarms a ridge, closed form ───────
    // The same low ridge is placed near (x²c small: its reach stays positive
    // and shadows a probe band) and far (x²c exceeds its height: reach < 0,
    // running_max_reach stays at the crossing's 0, the same-position probe
    // band stays visible). Only the curvature term distinguishes the two runs.
    {
        const double H = 0.75 * m.eye;  // low ridge; sub-horizon everywhere near

        // Far placement: just past 90% of the horizon — x²c > H there.
        const long k_far1 = std::lround(0.90 * x_horizon / m.s);
        const long k_far2 = k_far1 + 4;
        // Near placement: a few samples in — x²c tiny, ridge reach positive.
        const long k_near1 = 10, k_near2 = 14;
        // Probe band: behind the far ridge but still inside the horizon.
        const long k_p1 = k_far2 + 2;
        const long k_p2 = std::lround(0.97 * x_horizon / m.s);
        CHECK(k_p2 > k_p1 + 3, "setup: probe band spans several samples");

        const double x_far = m.x_of(static_cast<std::size_t>(k_far1));
        CHECK(H < x_far * x_far * m.c * 0.999,
              "setup: at the far placement x²c exceeds the ridge height");
        const double near_reach = m.feature_reach(H, k_near1);
        CHECK(near_reach > 0.0, "setup: the near ridge keeps a positive reach");
        for (long k = k_p1; k <= k_p2; ++k) {
            const double probe_reach = m.observer_reach(0.0, k);
            CHECK(probe_reach > 1.0,
                  "setup: the probe band is inside the bare curvature horizon");
            CHECK(probe_reach < near_reach - 1.0,
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
                  "curvature: moved out to where x²c swallows it, the SAME ridge "
                  "no longer blocks");
        }
        std::puts("PASS: x²c disarms a distant low ridge (closed-form curvature)");
    }

    // ── 6. Long Ridge regression (ADR-0016): high inland crest sees OVER a
    //      low coastal hill ─────────────────────────────────────────────────
    // The geometry class the secant model got wrong: a 200 m hill ~3 km
    // inland hides the NEAR-SHORE water from a 650 m crest ~19 km inland, but
    // the crest's own horizon reaches far beyond the hill's — the true sight
    // line to the ocean horizon clears the hill, so the crest is VISIBLE.
    // (Real-world instance: Long Ridge at 37.310858, −122.230542 blocked at
    // azimuth 300° by a 206 m hill near the San Gregorio coast.)
    {
        const double H_hill  = 200.0, H_crest = 650.0;
        const long   k_h1 = std::lround(3000.0 / m.s), k_h2 = k_h1 + 4;
        const long   k_r1 = std::lround(19000.0 / m.s), k_r2 = k_r1 + 9;
        const double x_hill  = m.x_of(static_cast<std::size_t>(k_h1));
        const double x_crest = m.x_of(static_cast<std::size_t>(k_r1));

        // The discriminating setup: the SECANT from the crossing to the crest
        // eye passes below the hill top (a secant-family test blocks this
        // observer even before curvature, which only worsens the far point)...
        CHECK((H_crest + m.eye) / x_crest < H_hill / x_hill,
              "setup: the coast-referenced secant is blocked by the hill");
        // ...while the crest out-reaches the hill decisively (tangent clears).
        CHECK(m.observer_reach(H_crest, k_r1) >
                  m.feature_reach(H_hill, k_h1) + 100.0,
              "setup: the crest's horizon reach beats the hill's with margin");

        const double h_lo = m.lon_lo(k_h1, box), h_hi = m.lon_hi(k_h2, box);
        const double r_lo = m.lon_lo(k_r1, box), r_hi = m.lon_hi(k_r2, box);
        FakeDEM dem([=](double, double lon) -> float {
            if (lon >= h_lo && lon <= h_hi) return static_cast<float>(H_hill);
            if (lon >= r_lo && lon <= r_hi) return static_cast<float>(H_crest);
            return 0.0f;
        });
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        const auto& ray = run_ray(engine, slice);

        // The flat between hill and crest is in the hill's shadow (probe a
        // band well interior to both features)...
        for (long k = k_h2 + 50; k <= k_r1 - 50; k += 100) {
            CHECK(m.observer_reach(0.0, k) < m.feature_reach(H_hill, k_h1) - 1.0,
                  "setup: the mid flat is out-reached by the hill");
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 0,
                  "long ridge: the low flat behind the hill stays blocked");
        }
        // ...but every crest sample is visible: the ocean horizon stands
        // above the hill as seen from 650 m.
        for (long k = k_r1; k <= k_r2; ++k) {
            CHECK(m.observer_reach(H_crest, k) >
                      m.feature_reach(H_crest, k_r1) + 10.0,
                  "setup: the crest never falls into its own rim's shadow");
            CHECK(ray.verdicts[static_cast<std::size_t>(k)] == 1,
                  "long ridge: the high inland crest must see over the "
                  "low coastal hill (ADR-0016)");
        }
        std::puts("PASS: a high inland crest sees the horizon over a low "
                  "coastal hill");
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
