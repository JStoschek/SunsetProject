#include "DEMTileLoader.h"
#include <gdal_priv.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

static fs::path make_temp_dir() {
    char tmpl[] = "/tmp/dem_lru_XXXXXX";
    char* result = mkdtemp(tmpl);
    assert(result);
    return fs::path(result);
}

// 1×1 synthetic tile. Pixel centre is at (lat_n − 0.5, −lon_w + 0.5).
static void make_tile(const fs::path& dir, int lat_n, int lon_w, float value) {
    gdal_init();
    std::string name = "USGS_13_n" + std::to_string(lat_n) +
                       "w" + std::to_string(lon_w) + "_synthetic.tif";
    fs::path path = dir / name;
    if (fs::exists(path)) fs::remove(path);

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = drv->Create(path.string().c_str(), 1, 1, 1, GDT_Float32, nullptr);
    assert(ds);

    double gt[6] = { (double)-lon_w, 1.0, 0.0,
                     (double)lat_n,  0.0, -1.0 };
    ds->SetGeoTransform(gt);

    GDALRasterBand* band = ds->GetRasterBand(1);
    band->RasterIO(GF_Write, 0, 0, 1, 1, &value, 1, 1, GDT_Float32, 0, 0);
    GDALClose(ds);
}

int main() {
    // --- Test 1: eviction + reload returns correct (updated) value ---
    // capacity=1: loading B evicts A; re-querying A must reload from the updated disk file.
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 100.0f);
        make_tile(dir, 39, 122, 200.0f);

        DEMTileLoader loader(dir.string(), /*lru_capacity=*/1);

        float a_before = loader.get_elevation(37.5, -121.5);
        assert(a_before == 100.0f);

        loader.get_elevation(38.5, -121.5); // B loads, A is evicted

        make_tile(dir, 38, 122, 999.0f); // update A on disk after eviction

        float a_after = loader.get_elevation(37.5, -121.5);
        assert(a_after == 999.0f);

        fs::remove_all(dir);
        std::puts("PASS: eviction + reload returns correct value");
    }

    // --- Test 2: reloaded value matches pre-eviction value ---
    // capacity=1: evict A via B, reload A without changing disk. Reload must equal original.
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 750.0f);
        make_tile(dir, 39, 122, 850.0f);

        DEMTileLoader loader(dir.string(), 1);

        float a_before = loader.get_elevation(37.5, -121.5);
        loader.get_elevation(38.5, -121.5); // evicts A
        float a_after  = loader.get_elevation(37.5, -121.5);

        assert(a_after == a_before);

        fs::remove_all(dir);
        std::puts("PASS: reloaded value matches pre-eviction value");
    }

    // --- Test 3: LRU order — A (the least recently used) is evicted, not B ---
    // capacity=2: load A, B, C in order; A is LRU when C loads; B must survive.
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 100.0f); // A
        make_tile(dir, 39, 122, 200.0f); // B
        make_tile(dir, 40, 122, 300.0f); // C

        DEMTileLoader loader(dir.string(), 2);

        loader.get_elevation(37.5, -121.5); // load A; lru=[A]
        loader.get_elevation(38.5, -121.5); // load B; lru=[B,A]
        loader.get_elevation(39.5, -121.5); // load C; A evicted; lru=[C,B]

        // Update both tiles on disk so we can tell which one reloads.
        make_tile(dir, 38, 122, 999.0f);
        make_tile(dir, 39, 122, 888.0f);

        // B was not evicted — still in cache, returns old 200.
        // (Query B before A to avoid B being evicted when A reloads.)
        float b_val = loader.get_elevation(38.5, -121.5);
        assert(b_val == 200.0f);

        // A was evicted when C loaded — reloads from updated disk, returns 999.
        float a_val = loader.get_elevation(37.5, -121.5);
        assert(a_val == 999.0f);

        fs::remove_all(dir);
        std::puts("PASS: LRU order — correct tile evicted");
    }

    // --- Test 4: re-accessed tile is promoted; the true LRU tile is evicted ---
    // capacity=2: load A, B; re-access A (B becomes LRU); load C → evicts B, not A.
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 100.0f); // A
        make_tile(dir, 39, 122, 200.0f); // B
        make_tile(dir, 40, 122, 300.0f); // C

        DEMTileLoader loader(dir.string(), 2);

        loader.get_elevation(37.5, -121.5); // load A; lru=[A]
        loader.get_elevation(38.5, -121.5); // load B; lru=[B,A]
        loader.get_elevation(37.5, -121.5); // re-access A; lru=[A,B], B=LRU
        loader.get_elevation(39.5, -121.5); // load C; B evicted; lru=[C,A]

        // Update both tiles on disk.
        make_tile(dir, 38, 122, 999.0f);
        make_tile(dir, 39, 122, 888.0f);

        // A was not evicted (it was MRU when C loaded) — returns old cached 100.
        // Query A first while it is still in cache.
        float a_val = loader.get_elevation(37.5, -121.5);
        assert(a_val == 100.0f);

        // B was evicted (it was LRU when C loaded) — reloads from disk, returns 888.
        float b_val = loader.get_elevation(38.5, -121.5);
        assert(b_val == 888.0f);

        fs::remove_all(dir);
        std::puts("PASS: re-accessed tile survives; LRU tile is evicted");
    }

    // --- Test 5: repeated queries do not evict the cached tile ---
    // capacity=1: querying the same tile many times must keep it in cache (no reload).
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 100.0f);
        make_tile(dir, 39, 122, 200.0f);

        DEMTileLoader loader(dir.string(), 1);

        loader.get_elevation(37.5, -121.5); // load A into cache

        // Update A on disk. A is still in cache, so queries must return the old value.
        make_tile(dir, 38, 122, 999.0f);

        for (int i = 0; i < 5; ++i)
            assert(loader.get_elevation(37.5, -121.5) == 100.0f);

        // Loading B now evicts A; next query of A picks up the updated disk value.
        loader.get_elevation(38.5, -121.5);
        assert(loader.get_elevation(37.5, -121.5) == 999.0f);

        fs::remove_all(dir);
        std::puts("PASS: repeated queries do not evict the cached tile");
    }

    // --- Test 6: lru_capacity defaults to 8 ---
    // 9 tiles: loading the 9th evicts tile 1 (LRU) but not tile 2.
    {
        auto dir = make_temp_dir();
        for (int i = 0; i < 9; ++i)
            make_tile(dir, 38 + i, 122, (float)((i + 1) * 100));

        DEMTileLoader loader(dir.string()); // no lru_capacity → defaults to 8

        // Load the first 8 tiles in order (fills cache to capacity).
        // After each get_elevation the queried tile becomes MRU; tile 1 ends up LRU.
        for (int i = 0; i < 8; ++i)
            assert(loader.get_elevation(37.5 + i, -121.5) == (float)((i + 1) * 100));

        // Update tiles 1 and 2 on disk so we can detect which one reloads.
        make_tile(dir, 38, 122, 999.0f); // tile 1 (LRU) updated
        make_tile(dir, 39, 122, 998.0f); // tile 2 updated

        // Load tile 9 (n46w122): cache is full, evicts tile 1 (LRU).
        assert(loader.get_elevation(45.5, -121.5) == 900.0f);

        // Tile 2 was not evicted — still in cache, returns old 200.
        // Query tile 2 before tile 1 to prevent tile 2 from being evicted when tile 1 reloads.
        float tile2_val = loader.get_elevation(38.5, -121.5);
        assert(tile2_val == 200.0f);

        // Tile 1 was evicted by the load of tile 9 — reloads from updated disk, returns 999.
        float tile1_val = loader.get_elevation(37.5, -121.5);
        assert(tile1_val == 999.0f);

        fs::remove_all(dir);
        std::puts("PASS: lru_capacity defaults to 8");
    }

    return 0;
}
