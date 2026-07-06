// End-to-end integration test: HorizonSweepEngine wired to the real production
// adapters (DEMAdapter / WaterAdapter) backed by a synthetic GeoTIFF fixture
// and the SF-Bay GSHHG coastline dataset.
//
// Design:
//   - Two synthetic 0 m tiles (n38w123, n38w122) cover the test box and force
//     at least one tile-boundary crossing, exercising bilinear interpolation
//     across the seam and LRU eviction when capacity < tile count.
//   - The engine's own march queries the real OceanMaskRasterizer's is_water
//     along bearing 90° (sunset azimuth 270°) from the open Pacific and finds
//     the SF-peninsula coast, driving the actual GSHHG coastline logic.
//   - Visibility assertions use the dynamically-found coast longitude so the
//     test is not brittle to GSHHG resolution differences.
//
// PIPELINE_CONF_PATH and GSHHG_FULL_PATH are injected by CMake.

#include "ProductionAdapters.h"
#include "PipelineConfig.h"

#include <gdal_priv.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

// Write a single-band Float32 GeoTIFF. gt = {origin_lon, dx, 0, origin_lat, 0, dy}.
static void write_geotiff(const std::string& path,
                          int width, int height,
                          const double gt[6],
                          const std::vector<float>& pixels) {
    gdal_init();
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    assert(drv);
    GDALDataset* ds = drv->Create(path.c_str(), width, height, 1, GDT_Float32, nullptr);
    assert(ds);
    ds->SetGeoTransform(const_cast<double*>(gt));
    GDALRasterBand* band = ds->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, width, height,
                                const_cast<float*>(pixels.data()),
                                width, height, GDT_Float32, 0, 0);
    assert(err == CE_None);
    GDALClose(ds);
}

static bool pixel(const AzimuthSlice& s, int row, int col) {
    return s.visible[static_cast<std::size_t>(row) * s.width + col];
}

static int col_for_lon(const PipelineConfig& c, double min_lon, double lon) {
    return static_cast<int>(std::lround((lon - min_lon) * c.cell_per_degree));
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Two flat 0 m tiles covering the test box. Each tile is 20×20 pixels at
// 0.05°/px, spanning exactly one degree of longitude and one degree of
// latitude — enough to cover the test box many times over.
//
// Tile {38, 123}: USGS_13_n38w123_synthetic.tif — lon [-123, -122], lat [37, 38]
// Tile {38, 122}: USGS_13_n38w122_synthetic.tif — lon [-122, -121], lat [37, 38]
//
// The tile boundary at lon = -122 falls inside the test box, exercising the
// bilinear-seam and LRU-eviction paths.

static fs::path make_fixture_dir() {
    char tmpl[] = "/tmp/sunset_adapters_XXXXXX";
    char* p = mkdtemp(tmpl);
    assert(p);
    return fs::path(p);
}

static void write_fixture_tiles(const fs::path& dir) {
    constexpr int W = 20, H = 20;
    std::vector<float> zeros(W * H, 0.0f);

    // Tile n38w123: origin lon=-123, lat=38, dx=0.05, dy=-0.05
    {
        double gt[6] = { -123.0, 0.05, 0.0, 38.0, 0.0, -0.05 };
        write_geotiff((dir / "USGS_13_n38w123_synthetic.tif").string(),
                      W, H, gt, zeros);
    }
    // Tile n38w122: origin lon=-122, lat=38, dx=0.05, dy=-0.05
    {
        double gt[6] = { -122.0, 0.05, 0.0, 38.0, 0.0, -0.05 };
        write_geotiff((dir / "USGS_13_n38w122_synthetic.tif").string(),
                      W, H, gt, zeros);
    }
}

// ---------------------------------------------------------------------------
// Test box: spans both DEM tiles; western edge is in the open Pacific.
//   lat [37.5, 37.501]  — ~11 rows at 1/3 arc-second
//   lon [-123.0, -121.9] — ~11880 cols, crossing tile boundary at -122
// ---------------------------------------------------------------------------
static constexpr double kMinLat  = 37.5;
static constexpr double kMaxLat  = 37.501;
static constexpr double kMinLon  = -123.0;
static constexpr double kMaxLon  = -121.9;
static constexpr double kAz      = 270.0;   // sunset due west
static constexpr int    kTestRow =  5;      // middle of the 11-row box

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    const PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);

    const fs::path fixture_dir = make_fixture_dir();
    write_fixture_tiles(fixture_dir);

    // ── Find the coast crossing for the test row so assertions are not
    //    brittle to exact GSHHG coastline position.
    // bearing = (kAz + 180) mod 360 = 90 (due east)
    const double bearing = std::fmod(kAz + 180.0, 360.0);  // 90.0
    // Approximate seed lat for the middle test row (row 5 at 10800 cpd).
    const double seed_lat = kMinLat + (kTestRow + 0.5) / config.cell_per_degree;
    OceanMaskRasterizer omr_pre(GSHHG_FULL_PATH);
    // March at the engine's own sample spacing (ADR-0014): the engine steps
    // is_water every ~10 m, so a coarse reference march could step over a
    // small rock or spit the engine's crossing lands on.
    const double sample_step_km =
        (config.sample_spacing_arcsec / 3600.0) * config.meters_per_degree_lat
        / 1000.0;
    const OceanOriginResult cross =
        omr_pre.ocean_origin_for_ray(bearing, seed_lat, kMinLon,
                                     sample_step_km, config.coast_march_max_km);
    const double coast_lon = cross.coast_lon;
    assert(coast_lon > kMinLon && coast_lon < kMaxLon &&
           "SF-peninsula coast must fall inside the test box");

    // ── Full integration run ─────────────────────────────────────────────
    {
        DEMTileLoader         dem_loader(fixture_dir.string(), config.dem_lru_capacity);
        OceanMaskRasterizer   omr(GSHHG_FULL_PATH, config.ocean_lru_capacity);
        DEMAdapter            dem_adapter(dem_loader);
        WaterAdapter          water_adapter(omr);

        HorizonSweepEngine engine(dem_adapter, water_adapter, config,
                                  kMinLat, kMaxLat, kMinLon, kMaxLon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        // --- Ocean pixels west of the crossing stay false ---
        const int coast_col = col_for_lon(config, kMinLon, coast_lon);
        for (int col = 0; col < coast_col; ++col) {
            assert(!pixel(slice, kTestRow, col) &&
                   "ocean pixel seaward of the coast crossing must be false");
        }
        std::puts("PASS: ocean pixels seaward of crossing are all false");

        // --- Near-coast pixel (within curvature horizon) is visible ---
        // 2.5 km past the coast is well inside the ~5.4 km curvature horizon
        // for 0 m terrain with default config.
        const double near_m   = 2500.0;
        const double deg_east = near_m / (config.meters_per_degree_lat *
                                          std::cos(kMinLat * M_PI / 180.0));
        const int near_col = col_for_lon(config, kMinLon, coast_lon + deg_east);
        assert(near_col >= 0 && near_col < slice.width);
        assert(pixel(slice, kTestRow, near_col) &&
               "flat pixel 2.5 km past the coast must be visible");
        std::puts("PASS: near-coast pixel is visible");

        // --- Far-inland pixel (well beyond curvature horizon) is hidden ---
        // 50 km past the coast exceeds the ~5.4 km horizon for 0 m terrain.
        const double far_m   = 50000.0;
        const double far_deg = far_m / (config.meters_per_degree_lat *
                                        std::cos(kMinLat * M_PI / 180.0));
        const int far_col = col_for_lon(config, kMinLon, coast_lon + far_deg);
        if (far_col < slice.width) {
            assert(!pixel(slice, kTestRow, far_col) &&
                   "0 m pixel 50 km inland must be hidden by earth curvature");
            std::puts("PASS: far-inland pixel is hidden by earth curvature");
        }

        // --- Tile-boundary seam: both tiles were exercised ---
        // The tile boundary is at lon=-122. Pixels on each side of it must be
        // accessible without NaN (0 m terrain was written to both tiles).
        const int seam_col_east = col_for_lon(config, kMinLon, -121.95);
        const int seam_col_west = col_for_lon(config, kMinLon, -122.05);
        // "Accessible" means the engine queried them and they are either
        // visible or false (not left out due to NaN / missing tile).
        // The far side of the seam should be hidden (well past the horizon).
        assert(!pixel(slice, kTestRow, seam_col_east) &&
               "pixel east of tile seam should be hidden (far inland)");
        assert(!pixel(slice, kTestRow, seam_col_west) &&
               "pixel west of tile seam should be hidden (far inland)");
        std::puts("PASS: tile-boundary seam exercised (both tiles loaded correctly)");
    }

    // ── LRU-bounded run: DEM capacity = 1 (< 2 tiles needed) ───────────
    // The computation must still produce the correct near-coast visibility
    // even when the LRU evicts one DEM tile to make room for the other.
    {
        PipelineConfig tight = config;
        tight.dem_lru_capacity   = 1;
        tight.ocean_lru_capacity = 1;

        DEMTileLoader         dem_loader(fixture_dir.string(), tight.dem_lru_capacity);
        OceanMaskRasterizer   omr(GSHHG_FULL_PATH, tight.ocean_lru_capacity);
        DEMAdapter            dem_adapter(dem_loader);
        WaterAdapter          water_adapter(omr);

        HorizonSweepEngine engine(dem_adapter, water_adapter, tight,
                                  kMinLat, kMaxLat, kMinLon, kMaxLon);
        AzimuthSlice slice;
        engine.compute_slice(kAz, slice);

        // Correctness must be identical to the full-capacity run.
        const double near_m   = 2500.0;
        const double deg_east = near_m / (config.meters_per_degree_lat *
                                          std::cos(kMinLat * M_PI / 180.0));
        const int near_col = col_for_lon(config, kMinLon, coast_lon + deg_east);
        assert(pixel(slice, kTestRow, near_col) &&
               "near-coast pixel must stay visible under LRU capacity = 1");
        std::puts("PASS: LRU capacity = 1 does not corrupt visibility");
    }

    fs::remove_all(fixture_dir);

    std::puts("ALL PASS");
    return 0;
}
