#include "DEMTileLoader.h"
#include <gdal_priv.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

static fs::path make_temp_dir() {
    char tmpl[] = "/tmp/dem_bilinear_XXXXXX";
    char* result = mkdtemp(tmpl);
    assert(result);
    return fs::path(result);
}

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

int main() {
    // --- Test 1: bilinear midpoint between four pixel centres ---
    // 4×4 tile, pixel size 0.25°. Query midway between (col=0,row=0), (1,0), (0,1), (1,1).
    // tx = ty = 0.5 → result = arithmetic mean of the four corner values.
    {
        auto dir = make_temp_dir();
        std::vector<float> px = {
            100.0f, 200.0f, 300.0f, 400.0f,
            110.0f, 210.0f, 310.0f, 410.0f,
            120.0f, 220.0f, 320.0f, 420.0f,
            130.0f, 230.0f, 330.0f, 430.0f,
        };
        // Standard tile: gt[0]=-122, dx=0.25, gt[3]=38, dy=-0.25 (4 pixels/degree).
        make_tile(dir, 38, 122, 4, 4, px, -122.0, 0.25, 38.0, -0.25);

        DEMTileLoader loader(dir.string());

        // Midpoint between (0,0),(1,0),(0,1),(1,1) centres:
        // lon = (-121.875 + -121.625)/2 = -121.75
        // lat = (37.875 + 37.625)/2     =  37.75
        float expected = (100.0f + 200.0f + 110.0f + 210.0f) / 4.0f; // 155.0f
        float got = loader.get_elevation(37.75, -121.75);
        assert(std::fabs(got - expected) < 0.001f);

        fs::remove_all(dir);
        std::puts("PASS: bilinear midpoint");
    }

    // --- Test 2: pixel-centre regression ---
    // Every pixel centre must still return that pixel's exact value (regression from issue 02),
    // including the last pixel (col=3, row=3) where clamping is required.
    {
        auto dir = make_temp_dir();
        std::vector<float> px = {
            100.0f, 200.0f, 300.0f, 400.0f,
            110.0f, 210.0f, 310.0f, 410.0f,
            120.0f, 220.0f, 320.0f, 420.0f,
            130.0f, 230.0f, 330.0f, 430.0f,
        };
        make_tile(dir, 38, 122, 4, 4, px, -122.0, 0.25, 38.0, -0.25);

        DEMTileLoader loader(dir.string());

        // (col=0, row=0) centre: lat=37.875, lon=-121.875
        assert(loader.get_elevation(37.875, -121.875) == 100.0f);
        // (col=2, row=1) centre
        assert(loader.get_elevation(37.625, -121.375) == 310.0f);
        // (col=3, row=3) — last pixel, needs clamped col1/row1
        assert(loader.get_elevation(37.125, -121.125) == 430.0f);

        fs::remove_all(dir);
        std::puts("PASS: pixel-centre regression");
    }

    // --- Test 3: tile boundary seam ---
    // Two adjacent tiles (n38w122 and n38w123), each 6 cols × 1 row, pixel width = 0.25°.
    // Each tile carries a 1-pixel overlap band whose values duplicate the adjacent tile.
    //   n38w122: gt[0]=-122.25 → col 0 (lon -122.125) and col 1 (lon -121.875) are the overlap.
    //   n38w123: gt[0]=-123.25 → col 4 (lon -122.125) and col 5 (lon -121.875) are the overlap.
    // The overlap values are set equal across both tiles so the bilinear result is seamless.
    // A query at the exact boundary (lon=-122) uses tile n38w122; a query 1 mm inside n38w123
    // uses that tile's overlap band and must return the same value (within 0.01f).
    {
        auto dir = make_temp_dir();

        // n38w122: 6 cols, 1 row, gt0=-122.25, dx=0.25, gt3=38, dy=-1
        std::vector<float> px_122 = { 500.0f, 600.0f, 700.0f, 800.0f, 900.0f, 1000.0f };
        make_tile(dir, 38, 122, 6, 1, px_122, -122.25, 0.25, 38.0, -1.0);

        // n38w123: 6 cols, 1 row, gt0=-123.25, dx=0.25, gt3=38, dy=-1
        // cols 4 and 5 duplicate n38w122 cols 0 and 1 (overlap band).
        std::vector<float> px_123 = { 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f };
        make_tile(dir, 38, 123, 6, 1, px_123, -123.25, 0.25, 38.0, -1.0);

        DEMTileLoader loader(dir.string());

        // Exact boundary: lon=-122 → owned by n38w122, tx=0.5, result=0.5*500+0.5*600=550.
        float at_boundary = loader.get_elevation(37.5, -122.0);
        assert(std::fabs(at_boundary - 550.0f) < 0.001f);

        // 1 mm inside n38w122 (≈1e-5°): same tile, result nearly identical.
        float in_122 = loader.get_elevation(37.5, -122.0 + 1e-5);
        assert(std::fabs(in_122 - 550.0f) < 0.01f);

        // 1 mm inside n38w123 (≈1e-5°): uses n38w123 overlap band (cols 4–5 = 500, 600).
        float in_123 = loader.get_elevation(37.5, -122.0 - 1e-5);
        assert(std::fabs(in_123 - 550.0f) < 0.01f);

        fs::remove_all(dir);
        std::puts("PASS: tile boundary seam");
    }

    return 0;
}
