#include "DEMTileLoader.h"
#include <gdal_priv.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

static fs::path make_temp_dir() {
    char tmpl[] = "/tmp/dem_test_XXXXXX";
    char* result = mkdtemp(tmpl);
    assert(result);
    return fs::path(result);
}

// Creates USGS_13_nLAT_wLON_synthetic.tif in dir.
// geotransform origin is NW corner: (x=-lon_w, y=lat_n), pixel size = 1/width × 1/height.
static void make_tile(const fs::path& dir, int lat_n, int lon_w,
                      int width, int height,
                      const std::vector<float>& pixels,
                      float nodata = -9999.0f) {
    gdal_init();
    std::string name = "USGS_13_n" + std::to_string(lat_n) +
                       "w" + std::to_string(lon_w) + "_synthetic.tif";
    std::string path = (dir / name).string();

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = drv->Create(path.c_str(), width, height, 1, GDT_Float32, nullptr);
    assert(ds);

    double gt[6] = { (double)-lon_w, 1.0 / width, 0.0,
                     (double)lat_n,  0.0,          -1.0 / height };
    ds->SetGeoTransform(gt);

    GDALRasterBand* band = ds->GetRasterBand(1);
    band->SetNoDataValue(nodata);
    band->RasterIO(GF_Write, 0, 0, width, height,
                   const_cast<float*>(pixels.data()),
                   width, height, GDT_Float32, 0, 0);
    GDALClose(ds);
}

int main() {
    // --- Test 1: pixel-centre lookup returns that pixel's elevation ---
    // 4×4 tile n38w122.  Geotransform: x0=-122, dx=0.25, y0=38, dy=-0.25.
    // Centre of pixel (col, row) = lon -122+(col+0.5)*0.25, lat 38-(row+0.5)*0.25
    {
        auto dir = make_temp_dir();
        std::vector<float> px = {
            100.0f, 200.0f, 300.0f, 400.0f,
            110.0f, 210.0f, 310.0f, 410.0f,
            120.0f, 220.0f, 320.0f, 420.0f,
            130.0f, 230.0f, 330.0f, 430.0f,
        };
        make_tile(dir, 38, 122, 4, 4, px);

        DEMTileLoader loader(dir.string());

        // (col=0, row=0) centre: lat=37.875, lon=-121.875
        assert(loader.get_elevation(37.875, -121.875) == 100.0f);
        // (col=2, row=1) centre: lat=37.625, lon=-121.375
        assert(loader.get_elevation(37.625, -121.375) == 310.0f);
        // (col=3, row=3) centre: lat=37.125, lon=-121.125
        assert(loader.get_elevation(37.125, -121.125) == 430.0f);

        fs::remove_all(dir);
        std::puts("PASS: pixel-centre lookup");
    }

    // --- Test 2: DEM no-data pixel returns exactly 0.0f ---
    // One pixel is set to the tile's nodata value; it must return 0.0f, not NaN.
    {
        auto dir = make_temp_dir();
        const float NODATA = -9999.0f;
        std::vector<float> px = {
            500.0f, NODATA,
            300.0f, 400.0f,
        };
        make_tile(dir, 38, 122, 2, 2, px, NODATA);

        DEMTileLoader loader(dir.string());

        // (col=1, row=0) is nodata: lon = -122+(1+0.5)*0.5 = -121.25, lat = 38-(0+0.5)*0.5 = 37.75
        assert(loader.get_elevation(37.75, -121.25) == 0.0f);
        // (col=0, row=0) is normal: should not be 0.0f
        assert(loader.get_elevation(37.75, -121.75) == 500.0f);

        fs::remove_all(dir);
        std::puts("PASS: nodata pixel returns 0.0f");
    }

    // --- Test 3: out-of-index coordinate returns NaN, not 0.0f ---
    // With a real tile in the index, a coordinate that falls outside it must
    // still return NaN so the two sentinels remain distinguishable.
    {
        auto dir = make_temp_dir();
        make_tile(dir, 38, 122, 2, 2, {100.0f, 200.0f, 300.0f, 400.0f});

        DEMTileLoader loader(dir.string());

        // lat 36 → tile key n37w..., not present in index
        float result = loader.get_elevation(36.5, -121.5);
        assert(std::isnan(result));
        assert(result != 0.0f);

        fs::remove_all(dir);
        std::puts("PASS: out-of-index returns NaN (not 0.0f)");
    }

    // --- Test 4: NaN and 0.0f are distinguishable ---
    // One loader returns both: nodata from an indexed tile → 0.0f,
    // missing tile → NaN.  Neither value must equal the other.
    {
        auto dir = make_temp_dir();
        const float NODATA = -9999.0f;
        make_tile(dir, 38, 122, 2, 2, {NODATA, 200.0f, 300.0f, 400.0f}, NODATA);

        DEMTileLoader loader(dir.string());

        float ocean = loader.get_elevation(37.75, -121.75); // nodata pixel
        float missing = loader.get_elevation(36.5, -121.5); // no tile

        assert(ocean == 0.0f);
        assert(std::isnan(missing));
        assert(ocean != missing);   // 0.0f != NaN

        fs::remove_all(dir);
        std::puts("PASS: NaN and 0.0f are distinguishable");
    }

    // --- Test 5: tile in index but unreadable → std::runtime_error with path ---
    // Place a zero-byte file that GDAL cannot open; the path must appear in
    // the exception message so operators can diagnose which tile is corrupt.
    {
        auto dir = make_temp_dir();
        std::string bad_name = "USGS_13_n38w122_synthetic.tif";
        std::string bad_path = (dir / bad_name).string();
        // Create an empty (corrupt) file
        { std::ofstream f(bad_path); }  // empty file, GDAL cannot open it

        DEMTileLoader loader(dir.string());

        bool threw = false;
        std::string msg;
        try {
            loader.get_elevation(37.5, -121.5);
        } catch (const std::runtime_error& e) {
            threw = true;
            msg   = e.what();
        }
        assert(threw);
        assert(msg.find(bad_path) != std::string::npos);

        fs::remove_all(dir);
        std::puts("PASS: unreadable tile throws runtime_error with path");
    }

    return 0;
}
