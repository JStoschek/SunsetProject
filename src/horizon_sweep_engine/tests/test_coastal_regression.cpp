// Coastal-regression test for the NW summer-sunset azimuth at Ocean Beach, SF.
//
// Motivation
// ----------
// Commit b350d3e replaced the exact geo_destination() coast-march in
// ocean_origin_for_ray with an incremental loxodrome approximation.  That
// shift moved the coastline crossing, changed along_c in the engine's
// rotated-frame calculation, and silently blacked out Ocean Beach at azimuths
// near 300° (max_az dropped to −inf / 286-291° instead of 300-301°).  The
// bug's own test suite passed because no test ran the engine with a coastal
// geometry matching the NW-sunset scenario and then checked that a beach cell
// was actually visible.  This file closes that gap.
//
// Design
// ------
//   • FakeWater: a meridional coast at lon = −122.511° (just west of Ocean
//     Beach's shoreline at ≈ −122.508°).  The engine's own march finds the
//     crossing (ADR-0014): the first sample at or east of the coast meridian,
//     so the crossing sits within one sample spacing of the exact coast.
//
//   • FakeDEM: all terrain at 0 m above sea level.  On flat zero terrain the
//     running_max_slope stays ≤ 0, so any cell within the curvature horizon
//     (≈ 5.4 km for 0 m terrain with default config) is visible.
//
//   • Azimuth: 300° (NW summer sunset, peak azimuth for San Francisco).
//     Bearing = (300 + 180) % 360 = 120° (SE, ocean → land direction).
//
//   • Key cell: Ocean Beach (lon ≈ −122.508°).  This cell is ≈ 265 m east of
//     the fake coast crossing, well inside the curvature horizon.  With flat
//     terrain it must be visible.  If along_c is mis-computed (or if the
//     crossing is shifted east past Ocean Beach), the cell's `dist` becomes
//     negative and it stays false — the exact b350d3e failure mode.
//
//   • Ocean Beach visibility is independent of the GSHHG dataset; it depends
//     only on the engine's along-ray distance calculation and the curvature
//     model.  Pinning it here ensures any future engine change that silently
//     shifts the coast/visibility boundary fails CI immediately.
//
// How this would have caught b350d3e
// ------------------------------------
// The b350d3e coast-march bug is pinned by Cycles 8-9 in
// test_ocean_origin_for_ray.cpp (which use the real GSHHG).  This test pins
// the ENGINE's behaviour: given a coast at a known longitude, does it correctly
// classify the 265 m-inland Ocean Beach cell as visible at az = 300°?  If the
// engine ever shifts along_c by more than 265 m (making OB seaward), this test
// fails.
//
// PIPELINE_CONF_PATH is injected by CMake.

#include "Fakes.h"
#include "PipelineConfig.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

// Coast at the Ocean Beach / Outer Sunset longitude (just west of the
// actual shoreline at ≈ −122.508°).  Matches what the production GSHHG
// march finds for this area at az = 300°.
constexpr double kCoastLon = -122.511;

// The Ocean Beach shoreline: 265 m east of kCoastLon, well inside the
// curvature horizon.  Must be visible at az = 300° with flat terrain.
constexpr double kOceanBeachLon = -122.508;

// Curvature horizon for 0 m terrain with the default config (eye 2 m,
// ADR-0016): x² (1-2k)/(2R) = eye → x ≈ 5.9 km past the coastline crossing.
// 1 km and 7 km are safely INTERIOR to the visible and hidden regions
// (re-anchored per ADR-0014: never assert on a boundary).
constexpr double kNearDistKm = 1.0;  // safely within horizon (dist_from_coast)
constexpr double kFarDistKm  = 7.0;  // safely beyond horizon

// Test box: one row tall (single ray, independent of the full rotated sweep),
// wide enough to contain both the coast crossing and 7 km of inland terrain.
//   row 0 is the only test row.
struct Box {
    double min_lat = 37.745;
    double max_lat = 37.7459;  // ≈ 1 row at 1/3 arc-second
    double min_lon = -123.0;   // open ocean west of SF
    double max_lon = -122.40;  // 6 km east of OB, beyond the curvature horizon
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

    // Sunset azimuth under test.
    constexpr double kAz = 300.0;   // NW summer sunset
    constexpr int    kRow = 0;      // only row in the single-row box

    // ── Ocean Beach cell is visible at az = 300° ────────────────────────────
    //
    // The coast at kCoastLon (−122.511°) places Ocean Beach 265 m inland.
    // With flat 0 m terrain the running_max_reach stays at 0 and the
    // curvature drop a few hundred metres inland is millimetres — far below
    // eye height.  The cell must be visible.
    //
    // b350d3e regression guard: if any future change to the coast march (or to
    // the engine's along_c calculation) shifts the crossing east past
    // kOceanBeachLon = −122.508°, the cell's dist becomes negative and the
    // engine skips it, leaving it false.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(kCoastLon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        const int ob_col   = col_for_lon(config, box, kOceanBeachLon);
        const int coast_col = col_for_lon(config, box, kCoastLon);

        assert(ob_col >= 0 && ob_col < slice.width &&
               "Ocean Beach column must fall inside the test box");
        assert(coast_col >= 0 && coast_col < slice.width &&
               "Coast-crossing column must fall inside the test box");

        // Ocean Beach is east of the coast → inland → must be visible.
        assert(pixel(slice, kRow, ob_col) &&
               "Ocean Beach cell (lon ≈ −122.508°) must be visible at az = 300°; "
               "a shift in along_c or the coast crossing may have made it seaward");
        std::puts("PASS: Ocean Beach cell is visible at az = 300° (NW summer sunset)");

        // Cells clearly west of the crossing are seaward → must be false.
        // NOTE: for diagonal bearings (az=300°, bearing=120°) the along-ray
        // boundary is not a clean longitude cut. Rays seeded slightly north of
        // the box have along_c just below the along of coast_col, so the ~10
        // cells immediately west of coast_col may have dist > 0 and are
        // correctly left visible by the engine. Check only cells at least 10
        // columns clear of the crossing meridian.
        for (int col = 0; col <= coast_col - 10; ++col) {
            assert(!pixel(slice, kRow, col) &&
                   "cell clearly west of coast crossing must be false");
        }
        std::puts("PASS: cells clearly west of crossing are false (seaward)");
    }

    // ── Near-coast cell (1 km inland) is visible ────────────────────────────
    // Within the curvature horizon, a flat 0 m cell must be visible.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(kCoastLon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        // 1 km east of the crossing in the bearing-120° direction.
        const double mpd = config.meters_per_degree_lat;
        const double near_deg = kNearDistKm * 1000.0 /
                                (mpd * std::cos(box.min_lat * M_PI / 180.0));
        const int near_col = col_for_lon(config, box, kCoastLon + near_deg);

        assert(near_col >= 0 && near_col < slice.width &&
               "near-coast column must fall inside the test box");
        assert(pixel(slice, kRow, near_col) &&
               "flat 0 m cell 1 km inland must be visible at az = 300°");
        std::puts("PASS: near-coast cell (1 km inland) is visible");
    }

    // ── Far-inland cell (7 km inland) is hidden by Earth curvature ──────────
    // At 7 km east of the coast (x ≈ 8.1 km along the tilted ray) the
    // curvature/refraction drop is ≈ 3.8 m > 2 m eye height → cell hidden.
    {
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(kCoastLon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        const double mpd = config.meters_per_degree_lat;
        const double far_deg = kFarDistKm * 1000.0 /
                               (mpd * std::cos(box.min_lat * M_PI / 180.0));
        const int far_col = col_for_lon(config, box, kCoastLon + far_deg);

        assert(far_col >= 0 && far_col < slice.width &&
               "far-inland column must fall inside the test box");
        assert(!pixel(slice, kRow, far_col) &&
               "flat 0 m cell 7 km inland must be hidden by Earth curvature at az = 300°");
        std::puts("PASS: far-inland cell (7 km) is hidden by Earth curvature");
    }

    // ── A ridge at the coast blocks the terrain behind it ───────────────────
    // With a 100 m ridge at the coast, the running_max_reach jumps to the
    // ridge's own horizon (≈ 41 km — far beyond any low observer's reach).
    // Ocean Beach (265 m behind the ridge) must be shadowed.  This exercises
    // the running_max_reach accumulation on a NW-bearing ray — a path not
    // covered by the cardinal (az = 270°) tests.
    {
        // Ridge at kCoastLon to kCoastLon+0.0002° (≈ 18 m wide), 100 m tall.
        FakeDEM dem_with_ridge([](double, double lon) {
            return (lon >= kCoastLon && lon <= kCoastLon + 0.0002) ? 100.0f : 0.0f;
        });
        FakeWater water(kCoastLon);
        HorizonSweepEngine engine(dem_with_ridge, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        const int ob_col = col_for_lon(config, box, kOceanBeachLon);
        assert(!pixel(slice, kRow, ob_col) &&
               "Ocean Beach must be shadowed by the 100 m coastal ridge at az = 300°");
        std::puts("PASS: coastal ridge at az = 300° shadows Ocean Beach");
    }

    // ── Negative along_c: coast near the western edge (heap-overflow guard) ──
    //
    // When the coast is near the western edge (lon ≈ −122.97°) and the bearing
    // is diagonal (120°), the seed for j_min is ~38.02°N — well outside the
    // box.  The march finds a crossing at Ec ≈ 0 and large Nc, making
    // along_c ≈ −12 000 m.  The verdict march then needs ~1 200 more entries
    // than the pre-allocation (sized for along_c = 0) provides.  Without the
    // per-ray resize guard this is a heap-buffer-overflow caught by
    // AddressSanitizer.
    //
    // Assertion: a flat 0 m cell ~500 m east of the crossing (inside the
    // curvature horizon) must be visible at az = 300°.
    {
        constexpr double kWestCoastLon = -122.97;
        FakeDEM   dem([](double, double) { return 0.0f; });
        FakeWater water(kWestCoastLon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);  // heap-buffer-overflow without the fix

        // Cell ~500 m east of the coast (kWestCoastLon + 0.005°), row 5.
        // The march finds a crossing near lat 37.747° for this ray;
        // dist ≈ 460 m → curvature drop ≈ 0.13 m < eye height → visible.
        const int inland_col = col_for_lon(config, box, kWestCoastLon + 0.005);
        assert(inland_col >= 0 && inland_col < slice.width &&
               "inland column must fall inside the test box");
        assert(pixel(slice, 5, inland_col) &&
               "flat 0 m cell ~500 m east of west-edge coast must be visible at az = 300°");
        std::puts("PASS: west-edge coast (negative along_c) — cell 500 m inland is visible");
    }

    std::puts("ALL PASS");
    return 0;
}
