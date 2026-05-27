// NaN / no-data robustness tests for HorizonSweepEngine::compute_slice.
// Tests drive the engine ONLY through compute_slice with FakeDEM / FakeCoast.
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

    // ── NaN in Phase 1 does not shadow terrain beyond the coverage gap ───
    // A band of NaN elevations in Phase 1 must not raise running_max_slope
    // (the sample contributes no slope and is effectively treated as 0 m).
    // A pixel just east of the gap, within the curvature horizon, must still
    // be visible — exactly as if the gap were flat sea level.
    {
        // Gap spans a wide inland band; everything else is 0 m.
        const double gap_lo = -122.93;
        const double gap_hi = -122.91;
        FakeDEM dem([gap_lo, gap_hi](double, double lon) -> float {
            if (lon >= gap_lo && lon <= gap_hi)
                return std::numeric_limits<float>::quiet_NaN();
            return 0.0f;
        });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row          = 5;
        // Just east of the gap, still within the curvature horizon (~3 km from coast).
        const int beyond_gap   = col_for_lon(config, box, -122.908);
        // Far inland — hidden by curvature regardless of the gap.
        const int far_col      = col_for_lon(config, box, -122.50);

        assert(pixel(slice, row, beyond_gap) &&
               "NaN Phase-1 gap must not shadow terrain beyond it");
        assert(!pixel(slice, row, far_col) &&
               "curvature still hides the far-inland pixel (sanity)");
        std::puts("PASS: NaN in Phase 1 does not shadow terrain beyond the coverage gap");
    }

    // ── A pixel with NaN own elevation is left false ─────────────────────
    // The observer position has no data: no observer can be recorded there.
    // A neighbouring pixel at the same distance with 0 m elevation is visible
    // (within the curvature horizon), confirming that only the NaN pixel is
    // suppressed — not the whole row.
    {
        const double nan_lon  = -122.940;  // ~0.9 km inland — within curvature horizon
        const double good_lon = -122.939;  // adjacent pixel, same distance
        FakeDEM dem([nan_lon](double, double lon) -> float {
            // Single column returns NaN; everything else is flat 0 m.
            return (std::fabs(lon - nan_lon) < 0.0002) ?
                   std::numeric_limits<float>::quiet_NaN() : 0.0f;
        });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);

        const int row      = 5;
        const int nan_col  = col_for_lon(config, box, nan_lon);
        const int good_col = col_for_lon(config, box, good_lon);

        assert(!pixel(slice, row, nan_col) &&
               "pixel with NaN own elevation must be left false");
        assert(pixel(slice, row, good_col) &&
               "adjacent 0 m pixel within the curvature horizon must be visible");
        std::puts("PASS: pixel with NaN own elevation is left false");
    }

    // ── No exception propagates when the DEM returns NaN everywhere ───────
    // compute_slice must complete normally even when every elevation query
    // returns NaN. The output buffer must be entirely false.
    {
        FakeDEM dem([](double, double) -> float {
            return std::numeric_limits<float>::quiet_NaN();
        });
        FakeCoast coast(-122.95);
        HorizonSweepEngine engine(dem, coast, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice slice;
        engine.compute_slice(kSunsetWest, slice);  // must not throw

        for (bool v : slice.visible) {
            assert(!v && "all-NaN DEM must yield an all-false output buffer");
        }
        std::puts("PASS: all-NaN DEM produces no exception and an all-false output");
    }

    std::puts("ALL PASS");
    return 0;
}
