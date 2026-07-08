// Behavioural specification for HorizonSweepEngine::compute_slice at the
// cardinal sunset azimuth (270° = due west). Tests drive the engine ONLY
// through compute_slice with FakeDEM / FakeWater — they never inspect
// engine internals, call private methods, or assert on query counts.
//
// Assertions sit safely INTERIOR to visible/blocked regions: under the
// forward-sampled grid (ADR-0014) output pixels inherit their nearest
// sample's verdict, so visibility boundaries may wobble by up to ~1 cell.
//
// PIPELINE_CONF_PATH is injected by CMake so the tests stay config-driven.
#include "Fakes.h"
#include "PipelineConfig.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

// Sunset azimuth under test: due west. At 270° each output row is one ray.
constexpr double kSunsetWest = 270.0;

// A test bounding box on the California coast. Spans ~0.6° of longitude so a
// far-inland pixel sits well beyond the curvature horizon, and a few rows of
// latitude so the ray-major sweep covers more than one ray.
struct Box {
    double min_lat = 37.0;
    double max_lat = 37.0009;   // ~10 rows at 1/3 arc-second
    double min_lon = -123.0;
    double max_lon = -122.4;    // ~6480 cols
};

PipelineConfig load_config() { return PipelineConfig::load(PIPELINE_CONF_PATH); }

// Column index of a longitude, using the engine's own grid convention.
int col_for_lon(const PipelineConfig& c, const Box& b, double lon) {
    return static_cast<int>(std::lround((lon - b.min_lon) * c.cell_per_degree));
}

bool pixel(const AzimuthSlice& s, int row, int col) {
    return s.visible[static_cast<std::size_t>(row) * s.width + col];
}

}  // namespace

int main() {
    const PipelineConfig config = load_config();
    const Box box;

    // ── Flat terrain near the coast is visible; far is not ──────────────
    // On an all-0 m profile, running_max_reach stays 0 (the crossing's sea
    // baseline; 0 m terrain reaches no farther), so a pixel is visible iff its
    // eye height clears the curvature drop: eye >= x^2 (1-2k)/(2R), i.e. x
    // within sqrt(eye/c) ≈ 5.9 km of the coast. A pixel ~0.9 km past the
    // coast is inside that horizon; one ~40 km inland is not.
    {
        FakeDEM  dem([](double, double) { return 0.0f; });  // flat sea level
        FakeWater water(-122.95);                           // meridional coast
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row     = 5;
        const int near_col = col_for_lon(config, box, -122.94);  // ~0.9 km inland
        const int far_col  = col_for_lon(config, box, -122.50);  // ~40 km inland

        assert(pixel(slice, row, near_col) &&
               "flat pixel within the curvature horizon must be visible");
        assert(!pixel(slice, row, far_col) &&
               "far-inland sea-level pixel must NOT be visible");
        std::puts("PASS: flat terrain visible near the coast, hidden far inland");
    }

    // ── Ocean pixels within the box are left false ──────────────────────
    // Every pixel seaward of the coastline crossing represents open water
    // where no observer can stand; it must stay false regardless of terrain.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row       = 5;
        const int coast_col = col_for_lon(config, box, -122.95);
        for (int col = 0; col < coast_col; ++col) {
            assert(!pixel(slice, row, col) &&
                   "ocean pixel west of the coastline crossing must be false");
        }
        std::puts("PASS: ocean pixels west of the crossing are left false");
    }

    // ── Single ridge blocks the flat terrain behind it ──────────────────
    // A 100 m ridge ~2 km past the coast raises running_max_reach. A flat
    // sea-level pixel further inland (whose own horizon reach falls short of
    // the ridge's) is shadowed; pixels at or before the ridge (within the
    // curvature horizon) remain visible — including the ridge crest, which
    // sees over itself because the eye-height offset is added at the check.
    {
        FakeDEM dem([](double, double lon) {
            return (lon >= -122.928 && lon <= -122.927) ? 100.0f : 0.0f;
        });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row        = 5;
        const int before_col = col_for_lon(config, box, -122.9388);  // ~1 km, before ridge
        const int crest_col  = col_for_lon(config, box, -122.9275);  // on the ridge
        const int beyond_col = col_for_lon(config, box, -122.9163);  // ~3 km, behind ridge

        assert(pixel(slice, row, before_col) &&
               "flat pixel before the ridge (within horizon) must be visible");
        assert(pixel(slice, row, crest_col) &&
               "the ridge crest sees over itself and must be visible");
        assert(!pixel(slice, row, beyond_col) &&
               "flat pixel behind the ridge must be shadowed");
        std::puts("PASS: a single ridge shadows the flat terrain behind it");
    }

    // ── An elevated observer sees over the ridge ────────────────────────
    // Behind the same 100 m ridge, the ground steps up to a 200 m shelf ~3 km
    // inland. The shelf's horizon out-reaches the ridge's, so the observer's
    // reach clears the ridge's running_max_reach and the observer is visible.
    // A flat pixel between the ridge and the shelf stays
    // shadowed in the same slice — proving the ridge is still active and that
    // it is the observer's height (added only at the check), not a special
    // case, that gets it over the ridge.
    {
        FakeDEM dem([](double, double lon) {
            if (lon >= -122.928 && lon <= -122.927) return 100.0f;  // ridge ~2 km
            if (lon >= -122.9163) return 200.0f;                    // shelf from ~3 km inland
            return 0.0f;
        });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row          = 5;
        // A few cells INSIDE the shelf (not its leading edge): the pixel must
        // gather from a sample that is itself on the shelf (ADR-0014 wobble).
        const int observer_col = col_for_lon(config, box, -122.9158);
        const int flat_col     = col_for_lon(config, box, -122.9220);  // flat, behind ridge

        assert(pixel(slice, row, observer_col) &&
               "elevated observer behind the ridge must be visible");
        assert(!pixel(slice, row, flat_col) &&
               "flat pixel behind the ridge is still shadowed");
        std::puts("PASS: an elevated observer sees over the ridge");
    }

    // ── Earth curvature hides a distant low observer ────────────────────
    // Pick a sea-level observer far enough out that the curvature/refraction
    // drop x^2(1-2k)/(2R) exceeds the eye height (ADR-0016: the observer's
    // horizon reach sqrt(eye/c) falls short of the coastline crossing). A
    // flat-earth model (no curvature term) would mark it visible — nothing
    // rises above eye level. With curvature, the observer is correctly hidden.
    {
        const double coast_lon = -122.95;
        const double obs_lon    = -122.848774;  // ~9 km inland
        const double cos_lat    = std::cos(box.min_lat * 3.14159265358979323846 / 180.0);
        const double east_m     = (obs_lon - coast_lon) * config.meters_per_degree_lat * cos_lat;
        const double drop       = east_m * east_m
                                  * (1.0 - 2.0 * config.refraction_coefficient_k)
                                  / (2.0 * config.earth_radius_m);

        // The mechanism: curvature drop outweighs eye height, yet flat-earth
        // (no drop, no terrain above eye level) would call it visible.
        assert(drop > config.observer_eye_height_m &&
               "test setup: curvature drop must exceed eye height at this distance");

        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(coast_lon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row     = 5;
        const int obs_col = col_for_lon(config, box, obs_lon);
        assert(!pixel(slice, row, obs_col) &&
               "curvature must hide the distant sea-level observer");
        std::puts("PASS: earth curvature hides a distant low observer");
    }

    // ── Eye-height offset lifts a pixel over the horizon ────────────────
    // At a distance where a 0 m pixel is beyond the curvature horizon (hidden,
    // with margin), ground raised by 1.7 m is visible: its own bare ground
    // enters the reach profile, and the eye-height offset added only at the
    // visibility check carries it over. Two identical runs, same coast, same
    // column, same distance — the only difference is the terrain height. The
    // distance sits between the bare horizon sqrt(eye/c) ≈ 5.9 km and the
    // raised horizon sqrt((1.7+eye)/c) ≈ 8.0 km so BOTH verdicts hold with a
    // comfortable margin (≈0.8 m each way), well clear of the
    // sample-quantisation wobble.
    {
        const double coast_lon = -122.95;
        const double obs_lon    = -122.8724;   // ~6.9 km inland
        FakeWater water(coast_lon);
        const int row     = 5;
        const int obs_col = col_for_lon(config, box, obs_lon);

        FakeDEM flat([](double, double) { return 0.0f; });
        HorizonSweepEngine flat_engine(flat, water, config,
                                       box.min_lat, box.max_lat,
                                       box.min_lon, box.max_lon);
        AzimuthSlice flat_slice;
        flat_engine.compute_slice(kSunsetWest, flat_slice);

        // Shelf starts just west of the sampled pixel (so rounding can't drop
        // the pixel onto bare sea level) but still far enough out that its own
        // reach is negative and never raises running_max_reach.
        FakeDEM raised([obs_lon](double, double lon) {
            return lon >= obs_lon - 0.001 ? 1.7f : 0.0f;  // ground at eye height
        });
        HorizonSweepEngine raised_engine(raised, water, config,
                                         box.min_lat, box.max_lat,
                                         box.min_lon, box.max_lon);
        AzimuthSlice raised_slice;
        raised_engine.compute_slice(kSunsetWest, raised_slice);

        assert(!pixel(flat_slice, row, obs_col) &&
               "0 m pixel at this distance must be hidden");
        assert(pixel(raised_slice, row, obs_col) &&
               "an otherwise-identical pixel at eye-height elevation must be visible");
        std::puts("PASS: eye-height offset lifts a pixel over the horizon");
    }

    // ── No ocean-cell rule: open sea never raises running_max_reach ─────
    // If open-sea cells (h = 0) raised running_max_reach, the very first land
    // pixel past the coast would be shadowed. Instead, on a flat profile every
    // pixel from the coastline crossing out to the curvature horizon is visible
    // as one contiguous band — no special case for water required.
    {
        const double coast_lon = -122.95;
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(coast_lon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row        = 5;
        const int coast_col  = col_for_lon(config, box, coast_lon);
        const int near_col   = col_for_lon(config, box, -122.918);  // ~2.8 km, inside horizon
        // Start a few columns inland: the march finds the crossing at sample
        // resolution, so the first land pixel or two may still gather a
        // seaward verdict (accepted ≤1-cell wobble, ADR-0014).
        for (int col = coast_col + 3; col <= near_col; ++col) {
            assert(pixel(slice, row, col) &&
                   "flat land from just past the coast out to the horizon must be visible");
        }
        std::puts("PASS: open sea raises no obstruction; coastal flat land is visible");
    }

    // ── The same buffer is sized once and refilled each call ────────────
    // compute_slice fills a caller-owned AzimuthSlice sized from the bbox and
    // cell resolution, and reuses it across calls: the buffer length is stable
    // and each call zeroes stale state before refilling.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        const int exp_width  = static_cast<int>(
            std::lround((box.max_lon - box.min_lon) * config.cell_per_degree));
        const int exp_height = static_cast<int>(
            std::lround((box.max_lat - box.min_lat) * config.cell_per_degree));

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        assert(slice.width == exp_width   && "width sized from bbox + cell size");
        assert(slice.height == exp_height && "height sized from bbox + cell size");
        assert(slice.min_lat == box.min_lat && slice.min_lon == box.min_lon &&
               "slice carries the box origin");
        assert(slice.visible.size() ==
                   static_cast<std::size_t>(exp_width) * exp_height &&
               "buffer sized to width * height");

        const std::size_t size_before = slice.visible.size();
        const int row      = 5;
        const int far_col  = col_for_lon(config, box, -122.50);   // far inland, hidden
        const int ocean_col = col_for_lon(config, box, -122.98);  // seaward of coast

        // Pollute the buffer with stale "visible" everywhere, then recompute.
        std::fill(slice.visible.begin(), slice.visible.end(), true);
        engine.compute_slice(kSunsetWest, slice);

        assert(slice.visible.size() == size_before &&
               "the same buffer is reused, not reallocated to a new size");
        assert(!pixel(slice, row, far_col) &&
               "stale visibility is cleared: far pixel is false again");
        assert(!pixel(slice, row, ocean_col) &&
               "stale visibility is cleared: ocean pixel is false again");
        std::puts("PASS: buffer is sized once and zeroed/refilled each call");
    }

    // ── Tunables are read from PipelineConfig, not hard-coded ───────────
    // A far-inland sea-level pixel hidden under the default config becomes
    // visible when eye height is raised, or when the refraction coefficient is
    // raised to cancel curvature — confirming the engine consults the config
    // object for these constants rather than baking them in.
    {
        const int row     = 5;
        const int far_col = col_for_lon(config, box, -122.50);  // ~40 km inland
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(-122.95);

        AzimuthSlice slice;

        {  // default config: far pixel is hidden
            HorizonSweepEngine engine(dem, water, config,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            engine.compute_slice(kSunsetWest, slice);
            assert(!pixel(slice, row, far_col) &&
                   "far sea-level pixel hidden under default tunables");
        }

        {  // raise eye height: the same pixel becomes visible
            PipelineConfig tall = config;
            tall.observer_eye_height_m = 1000.0;
            HorizonSweepEngine engine(dem, water, tall,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            engine.compute_slice(kSunsetWest, slice);
            assert(pixel(slice, row, far_col) &&
                   "eye height is read from config (raising it reveals the pixel)");
        }

        {  // cancel curvature via refraction coefficient: also becomes visible
            PipelineConfig flat_optics = config;
            flat_optics.refraction_coefficient_k = 0.5;  // (1 - 2k) = 0 -> no drop
            HorizonSweepEngine engine(dem, water, flat_optics,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            engine.compute_slice(kSunsetWest, slice);
            assert(pixel(slice, row, far_col) &&
                   "refraction coefficient is read from config (cancelling curvature reveals the pixel)");
        }
        std::puts("PASS: physical tunables come from PipelineConfig");
    }

    std::puts("ALL PASS");
    return 0;
}
