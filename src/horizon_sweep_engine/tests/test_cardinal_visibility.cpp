// Behavioural specification for HorizonSweepEngine::compute_slice at the
// cardinal sunset azimuth (270° = due west). Tests drive the engine ONLY
// through compute_slice with FakeDEM / FakeCoast — they never inspect
// running_max_slope, call private methods, or assert on query counts.
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
    // On an all-0 m profile, running_max_slope stays 0 (open sea has
    // non-positive slope), so a pixel is visible iff the 1.7 m eye height clears
    // the curvature drop: 1.7 >= d^2 (1-2k)/(2R), i.e. d within ~5.4 km. A pixel
    // ~0.9 km past the coast is inside that horizon; one ~40 km inland is not.
    {
        FakeDEM  dem([](double, double) { return 0.0f; });  // flat sea level
        FakeCoast coast(-122.95);                           // meridional coast
        HorizonSweepEngine engine(dem, coast, config,
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
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
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
    // A 100 m ridge ~2 km past the coast raises running_max_slope. A flat
    // sea-level pixel further inland (whose own h_adjusted/d falls below the
    // ridge's) is shadowed; pixels at or before the ridge (within the curvature
    // horizon) remain visible — including the ridge crest, which sees over
    // itself because the eye-height offset is added at the check.
    {
        FakeDEM dem([](double, double lon) {
            return (lon >= -122.928 && lon <= -122.927) ? 100.0f : 0.0f;
        });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
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
    // inland. The shelf's leading edge has a steeper slope than the ridge, so
    // (h_adjusted + eye_height)/d clears the ridge's running_max_slope and the
    // observer is visible. A flat pixel between the ridge and the shelf stays
    // shadowed in the same slice — proving the ridge is still active and that
    // it is the observer's height (added only at the check), not a special
    // case, that gets it over the ridge.
    {
        FakeDEM dem([](double, double lon) {
            if (lon >= -122.928 && lon <= -122.927) return 100.0f;  // ridge ~2 km
            if (lon >= -122.9163) return 200.0f;                    // shelf from ~3 km inland
            return 0.0f;
        });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row          = 5;
        const int observer_col = col_for_lon(config, box, -122.9163);  // shelf leading edge
        const int flat_col     = col_for_lon(config, box, -122.9220);  // flat, behind ridge

        assert(pixel(slice, row, observer_col) &&
               "elevated observer behind the ridge must be visible");
        assert(!pixel(slice, row, flat_col) &&
               "flat pixel behind the ridge is still shadowed");
        std::puts("PASS: an elevated observer sees over the ridge");
    }

    // ── Earth curvature hides a distant low observer ────────────────────
    // Pick a sea-level observer far enough out that the curvature/refraction
    // drop d^2(1-2k)/(2R) exceeds the eye height. A flat-earth model (no
    // curvature term) would mark it visible — running_max_slope is 0 and the
    // 1.7 m eye gives a positive slope. With curvature subtracted, the observer
    // falls below the horizon and is correctly hidden.
    {
        const double coast_lon = -122.95;
        const double obs_lon    = -122.848774;  // ~9 km inland
        const double cos_lat    = std::cos(box.min_lat * 3.14159265358979323846 / 180.0);
        const double east_m     = (obs_lon - coast_lon) * config.meters_per_degree_lat * cos_lat;
        const double d          = config.horizon_reference_offset_m + east_m;
        const double drop       = d * d * (1.0 - 2.0 * config.refraction_coefficient_k)
                                  / (2.0 * config.earth_radius_m);

        // The mechanism: curvature drop outweighs eye height, yet flat-earth
        // (no drop) would still see a positive slope and call it visible.
        assert(drop > config.observer_eye_height_m &&
               "test setup: curvature drop must exceed eye height at this distance");
        assert(config.observer_eye_height_m / d > 0.0 &&
               "flat-earth (no curvature) would mark this sea-level observer visible");

        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeCoast coast(coast_lon);
        HorizonSweepEngine engine(dem, coast, config,
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
    // At a distance where a 0 m pixel is just beyond the curvature horizon
    // (hidden), ground sitting at eye_height (1.7 m) is visible: its own bare
    // ground enters the slope profile, and the eye-height offset added only at
    // the visibility check carries it over. Two identical runs, same coast,
    // same column, same distance — the only difference is the terrain height.
    {
        const double coast_lon = -122.95;
        const double obs_lon    = -122.888138;  // ~5.5 km inland, just past horizon
        FakeCoast coast(coast_lon);
        const int row     = 5;
        const int obs_col = col_for_lon(config, box, obs_lon);

        FakeDEM flat([](double, double) { return 0.0f; });
        HorizonSweepEngine flat_engine(flat, coast, config,
                                       box.min_lat, box.max_lat,
                                       box.min_lon, box.max_lon);
        AzimuthSlice flat_slice;
        flat_engine.compute_slice(kSunsetWest, flat_slice);

        // Shelf starts just west of the sampled pixel (so rounding can't drop
        // the pixel onto bare sea level) but still far enough out that its own
        // slope is negative and never raises running_max_slope.
        FakeDEM raised([obs_lon](double, double lon) {
            return lon >= obs_lon - 0.001 ? 1.7f : 0.0f;  // ground at eye height
        });
        HorizonSweepEngine raised_engine(raised, coast, config,
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

    // ── No ocean-cell rule: open sea never raises running_max_slope ─────
    // If open-sea cells (h = 0) raised running_max_slope, the very first land
    // pixel past the coast would be shadowed. Instead, on a flat profile every
    // pixel from the coastline crossing out to the curvature horizon is visible
    // as one contiguous band — no special case for water required.
    {
        const double coast_lon = -122.95;
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeCoast coast(coast_lon);
        HorizonSweepEngine engine(dem, coast, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row        = 5;
        const int coast_col  = col_for_lon(config, box, coast_lon);
        const int near_col   = col_for_lon(config, box, -122.910);  // ~3.5 km, inside horizon
        for (int col = coast_col + 1; col <= near_col; ++col) {
            assert(pixel(slice, row, col) &&
                   "flat land from the coast out to the horizon must be visible");
        }
        std::puts("PASS: open sea raises no obstruction; coastal flat land is visible");
    }

    // ── The same buffer is sized once and refilled each call ────────────
    // compute_slice fills a caller-owned AzimuthSlice sized from the bbox and
    // cell resolution, and reuses it across calls: the buffer length is stable
    // and each call zeroes stale state before refilling.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
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
        FakeCoast coast(-122.95);

        AzimuthSlice slice;

        {  // default config: far pixel is hidden
            HorizonSweepEngine engine(dem, coast, config,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            engine.compute_slice(kSunsetWest, slice);
            assert(!pixel(slice, row, far_col) &&
                   "far sea-level pixel hidden under default tunables");
        }

        {  // raise eye height: the same pixel becomes visible
            PipelineConfig tall = config;
            tall.observer_eye_height_m = 1000.0;
            HorizonSweepEngine engine(dem, coast, tall,
                                      box.min_lat, box.max_lat,
                                      box.min_lon, box.max_lon);
            engine.compute_slice(kSunsetWest, slice);
            assert(pixel(slice, row, far_col) &&
                   "eye height is read from config (raising it reveals the pixel)");
        }

        {  // cancel curvature via refraction coefficient: also becomes visible
            PipelineConfig flat_optics = config;
            flat_optics.refraction_coefficient_k = 0.5;  // (1 - 2k) = 0 -> no drop
            HorizonSweepEngine engine(dem, coast, flat_optics,
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
