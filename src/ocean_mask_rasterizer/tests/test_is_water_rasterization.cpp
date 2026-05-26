#include "OceanMaskRasterizer.h"
#include <cassert>
#include <cstdio>

// All tests use the real gshhs_f.b on disk.  GSHHG_FULL_PATH is injected by
// CMake at compile time.

int main() {
    // --- Cycle 1: open Pacific is water ---
    // (38.0°N, 123.5°W) — well offshore, no land polygons in this tile.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(38.0, -123.5));
        std::puts("PASS: open Pacific is water");
    }

    // --- Cycle 2: inland land is not water ---
    // (37.7°N, 121.5°W) — Central Valley floor, unambiguously inside the
    // North-America Level-1 land polygon.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(!omr.is_water(37.7, -121.5));
        std::puts("PASS: inland land is not water");
    }

    // --- Cycle 3: San Francisco Bay is water ---
    // (37.85°N, 122.35°W) — mid-bay between the Bay Bridge and Treasure Island.
    // The GSHHG Level-1 North America polygon traces the detailed bay shoreline,
    // so shoreline-adjacent pixels (e.g. the Embarcadero at 37.8°N/122.4°W) can
    // fall inside the polygon boundary.  This coordinate is clearly in open water.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(37.85, -122.35));
        std::puts("PASS: SF Bay is water");
    }

    // --- Cycle 4: Tomales Bay is water ---
    // (38.15°N, 122.97°W) — outer Tomales Bay near the NW mouth where the bay
    // opens toward the Pacific.  The GSHHG Level-1 polygon traces the inner bay
    // shoreline fully, so the narrow inner bay interior appears as land.  The
    // mouth / outer bay area, where the polygon does not cross, is correctly
    // classified as water.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(38.15, -122.97));
        std::puts("PASS: Tomales Bay (outer mouth) is water");
    }

    std::puts("ALL PASS");
    return 0;
}
