// Unit tests for FrozenOcean / FrozenOceanView (parallel-azimuth-sweep Slice 3).
//
// Behaviors verified:
//   1. A frozen view's ocean_origin_for_ray and is_water match the stateful
//      OceanMaskRasterizer's for the same coordinates (the "reads match"
//      criterion) — exercised on the real Marin coastline.
//   2. The frozen structure spans the whole working set with no eviction, even
//      when it holds more tiles than the rasterizer's LRU capacity.
//   3. Concurrent coast-marches from multiple per-worker views are safe and
//      reproduce the single-threaded reference exactly.
//
// GSHHG_FULL_PATH is injected by CMake.

#include "FrozenOcean.h"
#include "OceanMaskRasterizer.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool same(const OceanOriginResult& a, const OceanOriginResult& b) {
    return a.coast_lat == b.coast_lat && a.coast_lon == b.coast_lon;
}

}  // namespace

int main() {
    // Near-shore Marin point; az 90° (due east) marches to the coast.  The march
    // stays within the tiles below.
    const double az  = 90.0;
    const double lat =  37.9;
    const double lon = -122.7;

    OceanMaskRasterizer omr(GSHHG_FULL_PATH);

    // Working set of GeoTiles (signed SW corner).
    std::set<GeoTile> work = {
        {37, -123}, {37, -124}, {37, -125},
    };
    FrozenOcean frozen = omr.freeze(work);

    // ── Cycle 1: frozen view reads match the stateful rasterizer ─────────────
    {
        FrozenOceanView view(frozen);

        const OceanOriginResult ref_omr = omr.ocean_origin_for_ray(az, lat, lon);
        const OceanOriginResult got_view = view.ocean_origin_for_ray(az, lat, lon);
        assert(same(ref_omr, got_view) &&
               "frozen view ocean_origin_for_ray must match the rasterizer");

        // is_water must agree across a grid of points inside the frozen tiles.
        for (int i = 0; i < 50; ++i) {
            const double qlat = 37.05 + 0.018 * i;          // 37.05 .. 37.93
            const double qlon = -123.95 + 0.018 * (i % 50); // sweeps tile -124
            assert(view.is_water(qlat, qlon) == omr.is_water(qlat, qlon) &&
                   "frozen view is_water must match the rasterizer");
        }
        std::puts("PASS: frozen ocean view reads match stateful rasterizer");
    }

    // ── Cycle 2: no eviction over a working set larger than the LRU ──────────
    // Freeze the three-tile set through a rasterizer whose LRU capacity is one;
    // all three must be resident and readable from the frozen structure.
    {
        OceanMaskRasterizer small_lru(GSHHG_FULL_PATH, /*lru_capacity=*/1);
        FrozenOcean frozen3 = small_lru.freeze(work);
        assert(frozen3.tile_count() == 3 && "all working-set tiles must be frozen");

        FrozenOceanView view(frozen3);
        // One point per frozen tile; each must match the stateful rasterizer
        // (which reloads under its capacity-1 LRU).
        const std::pair<double,double> per_tile[] = {
            {37.5, -122.5}, {37.5, -123.5}, {37.5, -124.5},
        };
        for (auto [qlat, qlon] : per_tile)
            assert(view.is_water(qlat, qlon) == small_lru.is_water(qlat, qlon));
        std::puts("PASS: no eviction over working set larger than LRU");
    }

    // ── Cycle 3: concurrent coast-marches from multiple views are safe ───────
    // Eight threads each drive their own FrozenOceanView and must reproduce the
    // single-threaded reference march + is_water reads exactly.  Under ASan this
    // also flags any data race on the shared bits or a view's cursor.
    {
        const OceanOriginResult ref = FrozenOceanView(frozen)
                                          .ocean_origin_for_ray(az, lat, lon);

        std::vector<std::pair<double,double>> probes;
        std::vector<bool> ref_water;
        for (int i = 0; i < 100; ++i) {
            double qlat = 37.1 + 0.008 * i;
            double qlon = -123.9 + 0.013 * i;
            probes.emplace_back(qlat, qlon);
        }
        {
            FrozenOceanView v(frozen);
            for (auto [qlat, qlon] : probes) ref_water.push_back(v.is_water(qlat, qlon));
        }

        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                FrozenOceanView view(frozen);   // each worker: its own cursor
                for (int rep = 0; rep < 20; ++rep) {
                    OceanOriginResult r = view.ocean_origin_for_ray(az, lat, lon);
                    if (!same(r, ref)) mismatches.fetch_add(1, std::memory_order_relaxed);
                    for (std::size_t i = 0; i < probes.size(); ++i)
                        if (view.is_water(probes[i].first, probes[i].second) != ref_water[i])
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads) th.join();
        assert(mismatches.load() == 0 && "concurrent view reads must match reference");
        std::puts("PASS: concurrent multi-view coast-marches are safe and correct");
    }

    fputs("ALL PASS\n", stdout);
    return 0;
}
