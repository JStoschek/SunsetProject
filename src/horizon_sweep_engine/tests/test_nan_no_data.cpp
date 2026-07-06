// NaN / no-data robustness tests for HorizonSweepEngine::compute_slice.
// Tests drive the engine ONLY through compute_slice with FakeDEM / FakeWater.
//
// ADR-0014 semantics: NaN elevation encountered during the march is treated as
// 0 m (ocean surface) — it neither shadows terrain behind it nor crashes the
// sweep. The engine does NOT special-case no-data output pixels: they inherit
// their nearest sample's verdict like any other pixel, and the downstream
// water/data mask (ADR-0013 Strip Processor) is responsible for masking them.
//
// PIPELINE_CONF_PATH is injected by CMake.
#include "Fakes.h"
#include "PipelineConfig.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr double kSunsetWest = 270.0;

struct Box {
    double min_lat = 37.0;
    double max_lat = 37.0009;
    double min_lon = -123.0;
    double max_lon = -122.4;
};

PipelineConfig load_config() { return PipelineConfig::load(PIPELINE_CONF_PATH); }

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

    // ── NaN during the march does not shadow terrain beyond the gap ──────
    // A band of NaN elevations must not raise running_max_slope (the sample is
    // treated as 0 m). A pixel just east of the gap, within the curvature
    // horizon, must still be visible — exactly as if the gap were flat sea
    // level.
    {
        // Gap spans a wide inland band; everything else is 0 m.
        const double gap_lo = -122.935;
        const double gap_hi = -122.925;
        FakeDEM dem([gap_lo, gap_hi](double, double lon) -> float {
            if (lon >= gap_lo && lon <= gap_hi)
                return std::numeric_limits<float>::quiet_NaN();
            return 0.0f;
        });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row          = 5;
        // Just east of the gap, still within the curvature horizon (~2.7 km from coast).
        const int beyond_gap   = col_for_lon(config, box, -122.920);
        // Far inland — hidden by curvature regardless of the gap.
        const int far_col      = col_for_lon(config, box, -122.50);

        assert(pixel(slice, row, beyond_gap) &&
               "NaN gap in the march must not shadow terrain beyond it");
        assert(!pixel(slice, row, far_col) &&
               "curvature still hides the far-inland pixel (sanity)");
        std::puts("PASS: NaN during the march does not shadow terrain beyond the gap");
    }

    // ── No-data pixels are NOT special-cased by the engine ───────────────
    // A pixel whose own elevation is NaN inherits its nearest sample's verdict
    // like any other pixel (the sample saw NaN -> 0 m -> visible within the
    // horizon). Masking no-data belongs to the downstream water/data mask
    // (ADR-0013), not the engine.
    {
        const double nan_lon  = -122.940;  // ~0.9 km inland — within curvature horizon
        const double good_lon = -122.939;  // adjacent pixel, same distance
        FakeDEM dem([nan_lon](double, double lon) -> float {
            // Single column returns NaN; everything else is flat 0 m.
            return (std::fabs(lon - nan_lon) < 0.0002) ?
                   std::numeric_limits<float>::quiet_NaN() : 0.0f;
        });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row      = 5;
        const int nan_col  = col_for_lon(config, box, nan_lon);
        const int good_col = col_for_lon(config, box, good_lon);

        assert(pixel(slice, row, nan_col) &&
               "NaN-elevation pixel inherits its sample's verdict (no engine special case)");
        assert(pixel(slice, row, good_col) &&
               "adjacent 0 m pixel within the curvature horizon is visible");
        std::puts("PASS: no-data pixels inherit sample verdicts (masked downstream)");
    }

    // ── No exception propagates when the DEM returns NaN everywhere ───────
    // compute_slice must complete normally even when every elevation query
    // returns NaN. All-NaN terrain behaves exactly like flat 0 m terrain:
    // visible from the coast out to the curvature horizon, hidden beyond.
    {
        FakeDEM dem([](double, double) -> float {
            return std::numeric_limits<float>::quiet_NaN();
        });
        FakeWater water(-122.95);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);  // must not throw

        const int row      = 5;
        const int near_col = col_for_lon(config, box, -122.94);  // ~0.9 km inland
        const int far_col  = col_for_lon(config, box, -122.50);  // ~40 km inland
        assert(pixel(slice, row, near_col) &&
               "all-NaN terrain behaves as flat 0 m: near-coast pixel visible");
        assert(!pixel(slice, row, far_col) &&
               "all-NaN terrain behaves as flat 0 m: far pixel hidden by curvature");
        std::puts("PASS: all-NaN DEM completes and behaves as flat 0 m terrain");
    }

    std::puts("ALL PASS");
    return 0;
}
