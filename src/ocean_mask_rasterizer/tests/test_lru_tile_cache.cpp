// LRU tile-cache tests for OceanMaskRasterizer.
// GSHHG_FULL_PATH is injected by CMake at compile time.
#include "OceanMaskRasterizer.h"
#include <cassert>
#include <cstdio>

int main() {
    // --- Cycle 1: cache hit — same tile served without re-rasterizing --------
    // Query (38.5°N, 123.5°W) twice.  Both hits the same 1°×1° tile
    // (floor_lat=38, floor_lon=-124).  Only the first query should rasterize.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        bool r1 = omr.is_water(38.5, -123.5);
        assert(omr.rasterize_count() == 1);
        bool r2 = omr.is_water(38.5, -123.5);
        assert(omr.rasterize_count() == 1); // cache hit — no re-rasterize
        assert(r1 == r2);
        std::puts("PASS: cache hit serves without re-rasterizing");
    }

    // --- Cycle 2: eviction at capacity=1 ------------------------------------
    // Query tile A (floor_lat=38, floor_lon=-124), then tile B (floor_lat=37,
    // floor_lon=-124).  At capacity 1, tile B evicts tile A.  A third query of
    // tile A must re-rasterize (rasterize_count bumps to 3).
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH, /*lru_capacity=*/1);
        omr.is_water(38.5, -123.5);              // tile A → rasterize_count=1
        assert(omr.rasterize_count() == 1);
        omr.is_water(37.5, -123.5);              // tile B → evicts A, count=2
        assert(omr.rasterize_count() == 2);
        omr.is_water(38.5, -123.5);              // tile A evicted → re-rasterize, count=3
        assert(omr.rasterize_count() == 3);
        std::puts("PASS: capacity=1 evicts tile A when tile B loads");
    }

    // --- Cycle 3: reloaded value matches pre-eviction value -----------------
    // Evict tile A via tile B (capacity=1), then reload tile A.  Rasterization
    // is deterministic so the reloaded result must equal the original.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH, /*lru_capacity=*/1);
        bool before = omr.is_water(38.5, -123.5); // tile A
        omr.is_water(37.5, -123.5);               // tile B evicts A
        bool after  = omr.is_water(38.5, -123.5); // tile A reloaded
        assert(before == after);
        std::puts("PASS: reloaded value matches pre-eviction value");
    }

    // --- Cycle 4: lru_capacity defaults to 8 --------------------------------
    // Load 9 distinct offshore Pacific tiles (floor_lat = 30..38, floor_lon=-124).
    // After loading all 9, the first tile (floor_lat=30) must have been evicted
    // (i.e. total rasterize_count == 10 after re-querying it), while tile 2
    // (floor_lat=31) is still cached (rasterize_count stays at 10 after re-query).
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH); // default capacity = 8

        // Load 9 tiles in order.  Each is a different 1°×1° Pacific tile.
        for (int lat = 30; lat <= 38; ++lat)
            omr.is_water(lat + 0.5, -123.5);

        assert(omr.rasterize_count() == 9);

        // Tile 2 (floor_lat=31) is still in cache — re-query must not rasterize.
        omr.is_water(31.5, -123.5);
        assert(omr.rasterize_count() == 9);

        // Tile 1 (floor_lat=30) was evicted when tile 9 loaded — re-query rasterizes.
        omr.is_water(30.5, -123.5);
        assert(omr.rasterize_count() == 10);

        std::puts("PASS: lru_capacity defaults to 8");
    }

    // --- Cycle 5: issue-02 correctness still holds with cache in place ------
    // Re-run the four geographic truth assertions.  These pass through the
    // LRU cache path (first query = miss, result cached) so they validate
    // both the cache wiring and the underlying rasterization.
    {
        // (38.0°N, 123.5°W) — open Pacific, no land polygons in tile.
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(38.0, -123.5));
        std::puts("PASS: open Pacific is water (via cache)");
    }
    {
        // (37.7°N, 121.5°W) — Central Valley floor, inside N-America polygon.
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(!omr.is_water(37.7, -121.5));
        std::puts("PASS: inland land is not water (via cache)");
    }
    {
        // (37.85°N, 122.35°W) — mid SF Bay, open water.
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(37.85, -122.35));
        std::puts("PASS: SF Bay is water (via cache)");
    }
    {
        // (38.15°N, 122.97°W) — outer Tomales Bay mouth.
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(38.15, -122.97));
        std::puts("PASS: Tomales Bay (outer mouth) is water (via cache)");
    }

    std::puts("ALL PASS");
    return 0;
}
