// Unit tests for FrozenDEM / FrozenDEMView (parallel-azimuth-sweep Slice 3).
//
// Behaviors verified:
//   1. A frozen view's reads match the stateful loader's reads for the same
//      coordinates (the agreed "reads match" criterion).
//   2. The frozen structure spans the whole working set — every tile in the set
//      is readable, and no eviction drops a tile regardless of how many are
//      frozen at once (more than the loader's LRU capacity).
//   3. Concurrent reads from multiple per-worker views are safe and correct.
//
// Uses synthetic GeoTIFF tiles (no dependency on the multi-GB real DEM).

#include "DEMTileLoader.h"
#include "FrozenDEM.h"

#include <gdal_priv.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

static fs::path make_temp_dir() {
    char tmpl[] = "/tmp/frozen_dem_XXXXXX";
    char* result = mkdtemp(tmpl);
    assert(result);
    return fs::path(result);
}

// Write a 1°×1° synthetic tile covering lon [lon_w-... ] — geographic floor
// (lat_n-1, -lon_w).  Pixels vary with position so different coordinates read
// distinct values.
static void make_tile(const fs::path& dir, int lat_n, int lon_w,
                      int width, int height,
                      const std::vector<float>& pixels,
                      double gt0, double dx, double gt3, double dy) {
    gdal_init();
    std::string name = "USGS_13_n" + std::to_string(lat_n) +
                       "w" + std::to_string(lon_w) + "_synthetic.tif";
    std::string path = (dir / name).string();
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = drv->Create(path.c_str(), width, height, 1, GDT_Float32, nullptr);
    assert(ds);
    double gt[6] = { gt0, dx, 0.0, gt3, 0.0, dy };
    ds->SetGeoTransform(gt);
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->RasterIO(GF_Write, 0, 0, width, height,
                   const_cast<float*>(pixels.data()),
                   width, height, GDT_Float32, 0, 0);
    GDALClose(ds);
}

// A 4×4 tile whose pixel values encode (tile, col, row) so reads are distinct.
static std::vector<float> ramp(float base) {
    std::vector<float> px(16);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            px[r * 4 + c] = base + 10.0f * r + c;
    return px;
}

int main() {
    auto dir = make_temp_dir();

    // Two adjacent 1° tiles: geographic floors (37,-122) and (37,-123).
    // n38w122 covers lon [-122,-121], lat [37,38]; n38w123 covers lon [-123,-122].
    make_tile(dir, 38, 122, 4, 4, ramp(1000.0f), -122.0, 0.25, 38.0, -0.25);
    make_tile(dir, 38, 123, 4, 4, ramp(2000.0f), -123.0, 0.25, 38.0, -0.25);

    DEMTileLoader loader(dir.string());

    // Working set of GeoTiles (what strip_working_set returns).
    std::set<GeoTile> work = {{37, -122}, {37, -123}};
    FrozenDEM frozen = loader.freeze(work);

    // ── Cycle 1: frozen view reads match the stateful loader ─────────────────
    {
        FrozenDEMView view(frozen);
        // Sample several coordinates across both tiles, including pixel centres.
        const std::pair<double,double> coords[] = {
            {37.875, -121.875}, {37.625, -121.375}, {37.125, -121.125},
            {37.75,  -121.75},  {37.875, -122.875}, {37.5,   -122.5},
        };
        for (auto [lat, lon] : coords) {
            float want = loader.get_elevation(lat, lon);
            float got  = view.get_elevation(lat, lon);
            assert(want == got && "frozen view read must match stateful loader");
        }
        std::puts("PASS: frozen view reads match stateful loader");
    }

    // ── Cycle 2: no eviction over a working set larger than the LRU ──────────
    // Freeze four tiles through a loader whose LRU capacity is only two; the
    // frozen structure must hold all four and read every one back.
    {
        auto dir4 = make_temp_dir();
        make_tile(dir4, 38, 122, 4, 4, ramp(1000.0f), -122.0, 0.25, 38.0, -0.25);
        make_tile(dir4, 38, 123, 4, 4, ramp(2000.0f), -123.0, 0.25, 38.0, -0.25);
        make_tile(dir4, 39, 122, 4, 4, ramp(3000.0f), -122.0, 0.25, 39.0, -0.25);
        make_tile(dir4, 39, 123, 4, 4, ramp(4000.0f), -123.0, 0.25, 39.0, -0.25);

        DEMTileLoader small_lru(dir4.string(), /*lru_capacity=*/2);
        std::set<GeoTile> work4 = {
            {37, -122}, {37, -123}, {38, -122}, {38, -123},
        };
        FrozenDEM frozen4 = small_lru.freeze(work4);
        assert(frozen4.tile_count() == 4 && "all working-set tiles must be frozen");

        FrozenDEMView view(frozen4);
        // One interior coordinate per tile — all four must read finite values
        // and match the stateful loader (which reloads as needed under its LRU).
        const std::pair<double,double> per_tile[] = {
            {37.5, -121.5}, {37.5, -122.5}, {38.5, -121.5}, {38.5, -122.5},
        };
        for (auto [lat, lon] : per_tile) {
            float got = view.get_elevation(lat, lon);
            assert(!std::isnan(got) && "every working-set tile must be readable");
            assert(got == small_lru.get_elevation(lat, lon));
        }
        fs::remove_all(dir4);
        std::puts("PASS: no eviction over working set larger than LRU");
    }

    // ── Cycle 3: concurrent reads from multiple per-worker views are safe ────
    // Eight threads each drive their own FrozenDEMView over the same shared,
    // immutable FrozenDEM and must reproduce the single-threaded reference reads
    // exactly.  Run under ThreadSanitizer/AddressSanitizer this also flags any
    // data race on the shared tile data or a view's cursor.
    {
        // Reference reads (single-threaded) over a grid spanning both tiles.
        std::vector<std::pair<double,double>> grid;
        for (int i = 0; i < 200; ++i) {
            double lon = -122.95 + 0.009 * (i % 100);  // sweeps both tiles
            double lat = 37.05 + 0.004 * (i % 100);
            grid.emplace_back(lat, lon);
        }
        std::vector<float> reference;
        reference.reserve(grid.size());
        {
            FrozenDEMView ref_view(frozen);
            for (auto [lat, lon] : grid)
                reference.push_back(ref_view.get_elevation(lat, lon));
        }

        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                FrozenDEMView view(frozen);   // each worker: its own cursor
                for (int rep = 0; rep < 50; ++rep) {
                    for (std::size_t i = 0; i < grid.size(); ++i) {
                        float got = view.get_elevation(grid[i].first, grid[i].second);
                        const bool ok = (got == reference[i]) ||
                                        (std::isnan(got) && std::isnan(reference[i]));
                        if (!ok) mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        assert(mismatches.load() == 0 && "concurrent view reads must match reference");
        std::puts("PASS: concurrent multi-view reads are safe and correct");
    }

    fs::remove_all(dir);
    std::puts("ALL PASS");
    return 0;
}
