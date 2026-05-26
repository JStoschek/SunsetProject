// Tests for the GSHHG spatial index built at OceanMaskRasterizer construction.
// The index maps each 1°×1° TileKey to the file offsets of Level-1 polygons
// that overlap it, so rasterize_tile() no longer scans the full file on a miss.
//
// GSHHG_FULL_PATH is injected by CMake at compile time.
#include "OceanMaskRasterizer.h"
#include <cassert>
#include <chrono>
#include <cstdio>

// ---------------------------------------------------------------------------
// Helper: elapsed wall-clock milliseconds for a callable.
// ---------------------------------------------------------------------------
template<typename F>
static long long ms(F&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

int main() {
    // --- Cycle 1: open-Pacific cache miss < 3 s --------------------------------
    // (38.0°N, 123.5°W) lies in tile (floor_lat=38, floor_lon=-124).  No
    // Level-1 polygons overlap this deep-ocean tile, so the index lookup returns
    // an empty list and rasterize_tile() does no polygon I/O at all.  With the
    // full-file scan (270 MB) this takes ~14 s; with the index it should be
    // well under 3 s.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        long long elapsed = ms([&]{ omr.is_water(38.0, -123.5); });
        std::printf("  open-Pacific cache miss: %lld ms\n", elapsed);
        assert(elapsed < 3000 && "open-Pacific cache miss must complete in under 3 s with spatial index");
        assert(omr.is_water(38.0, -123.5));   // correctness: still water
        std::puts("PASS: open-Pacific cache miss < 3 s");
    }

    // --- Cycle 2: all 4 truth-point rasterizations < 10 s total ---------------
    // The four geographic truth assertions each hit a different 1°×1° tile.
    // Combined, they must complete in under 10 s with the spatial index +
    // per-polygon tile clipping in place.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        bool r1, r2, r3, r4;
        long long total = ms([&]{
            r1 = omr.is_water(38.0,  -123.5);   // open Pacific    – tile (38,-124)
            r2 = omr.is_water(37.7,  -121.5);   // Central Valley  – tile (37,-122)
            r3 = omr.is_water(37.85, -122.35);  // SF Bay          – tile (37,-123)
            r4 = omr.is_water(38.15, -122.97);  // Tomales Bay     – tile (38,-123)
        });
        std::printf("  4 truth-point rasterizations: %lld ms\n", total);
        assert(total < 10000 &&
               "4 truth-point rasterizations must complete in under 10 s with spatial index");
        assert(r1  && "open Pacific must be water");
        assert(!r2 && "Central Valley must be land");
        assert(r3  && "SF Bay must be water");
        assert(r4  && "Tomales Bay (outer mouth) must be water");
        std::puts("PASS: 4 truth-point rasterizations < 10 s with correct answers");
    }

    // --- Cycle 3: geographic truth assertions still correct ------------------
    // Verify that clipping + index-based rasterization produces the same
    // correct water/land answers as the full-scan approach.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(38.0,  -123.5)  && "open Pacific must be water");
        std::puts("PASS: open Pacific is water");
        assert(!omr.is_water(37.7, -121.5)  && "Central Valley must be land");
        std::puts("PASS: Central Valley is land");
        assert(omr.is_water(37.85, -122.35) && "SF Bay mid-bay must be water");
        std::puts("PASS: SF Bay is water");
        assert(omr.is_water(38.15, -122.97) && "Tomales Bay outer mouth must be water");
        std::puts("PASS: Tomales Bay outer mouth is water");
    }

    std::puts("ALL PASS");
    return 0;
}
