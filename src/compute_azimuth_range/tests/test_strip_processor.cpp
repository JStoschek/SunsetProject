// Focused integration test for StripProcessor — the per-strip seam that owns
// working-set → freeze(DEM + ocean) → parallel sweep → water-mask.
//
// Fully synthetic, no GSHHG/OSM data files:
//   - DEM: one flat 0 m tile (USGS_13_n38w123) covering the test strip.
//   - Ocean: a FakeWaterPolygonSource whose land is everything east of a coast
//     meridian, so the strip has open Pacific to the west and flat land east.
//
// The strip is run at a coarse cell_per_degree (fast) and the result is checked
// against all three mask states the composition must produce:
//   1. a water pixel (west of the coast)            -> (NaN, NaN)
//   2. a land pixel just east of the coast (visible) -> finite range
//   3. a far-inland land pixel (curvature hides the
//      ocean horizon over flat terrain)             -> (+inf, -inf) sentinel
//
// PIPELINE_CONF_PATH is injected by CMake; tunables are overridden for speed.

#include "StripProcessor.h"

#include "DEMTileLoader.h"
#include "OceanMaskRasterizer.h"
#include "PipelineConfig.h"
#include "ThreadPool.h"
#include "WaterPolygonSource.h"

#include <gdal_priv.h>
#include <ogr_geometry.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

// A flat 0 m single-band GeoTIFF named to the USGS NW-corner convention.
static void write_flat_tile(const std::string& path, int n, int w, int px) {
    gdal_init();
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    assert(drv);
    GDALDataset* ds = drv->Create(path.c_str(), px, px, 1, GDT_Float32, nullptr);
    assert(ds);
    // NW corner = (lat n, lon -w); 1° span; north-up.
    const double step = 1.0 / px;
    double gt[6] = { (double)(-w), step, 0.0, (double)n, 0.0, -step };
    ds->SetGeoTransform(gt);
    std::vector<float> pixels(static_cast<std::size_t>(px) * px, 0.0f);
    GDALRasterBand* band = ds->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, px, px, pixels.data(),
                                px, px, GDT_Float32, 0, 0);
    assert(err == CE_None);
    GDALClose(ds);
}

// Land = everything east of `coast_lon`; west of it is ocean.  Returns one
// rectangle per tile (caller / rasterizer takes ownership).
class FakeWaterPolygonSource : public WaterPolygonSource {
public:
    explicit FakeWaterPolygonSource(double coast_lon) : coast_lon_(coast_lon) {}
    void land_polygons_for_tile(int tile_lat, int tile_lon,
                                std::vector<OGRPolygon*>& out) override {
        const double w = std::max((double)tile_lon, coast_lon_);
        const double e = (double)tile_lon + 1.0;
        if (w >= e) return;  // whole tile is ocean
        const double s = tile_lat, n = tile_lat + 1.0;
        OGRLinearRing* ring = new OGRLinearRing();
        ring->addPoint(w, s); ring->addPoint(e, s);
        ring->addPoint(e, n); ring->addPoint(w, n); ring->addPoint(w, s);
        OGRPolygon* poly = new OGRPolygon();
        poly->addRingDirectly(ring);
        out.push_back(poly);
    }
private:
    double coast_lon_;
};

int main() {
    gdal_init();

    // ── Synthetic DEM: one flat 0 m tile covering lat [37,38], lon [-123,-122]
    const fs::path dir = fs::temp_directory_path() /
        ("strip_proc_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    write_flat_tile((dir / "USGS_13_n38w123_synthetic.tif").string(), 38, 123, 120);

    // ── Config: real file, coarse + narrow for a fast strip ───────────────
    PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);
    config.cell_per_degree  = 60.0;   // 1 arc-min — tiny strip
    config.azimuth_min_deg  = 260.0;
    config.azimuth_max_deg  = 280.0;
    config.azimuth_step_deg = 5.0;    // 5 azimuths around due-west sunset
    config.worker_threads   = 2;

    const double coast_lon = -122.5;
    DEMTileLoader       dem(dir.string(), config.dem_lru_capacity);
    OceanMaskRasterizer omr(
        std::make_unique<FakeWaterPolygonSource>(coast_lon),
        config.ocean_lru_capacity);
    ThreadPool pool(config.worker_threads);

    // ── Strip: lon [-122.8, -122.2], lat [37.3, 37.5] ─────────────────────
    const double min_lon = -122.8, max_lon = -122.2;
    const double strip_min_lat = 37.3, strip_max_lat = 37.5;

    StripProcessor sp(dem, omr, pool, config, min_lon, max_lon);
    StripResult result = sp.process(strip_min_lat, strip_max_lat);

    const double cell_deg = 1.0 / config.cell_per_degree;
    const int W = result.width, H = result.height;
    assert(W == (int)std::lround((max_lon - min_lon) * config.cell_per_degree));
    assert(H == (int)std::lround((strip_max_lat - strip_min_lat) * config.cell_per_degree));
    std::printf("PASS: strip dims %d x %d\n", W, H);

    auto idx = [&](double lat, double lon) {
        const int r = (int)std::lround((lat - strip_min_lat) / cell_deg);
        const int c = (int)std::lround((lon - min_lon) / cell_deg);
        assert(r >= 0 && r < H && c >= 0 && c < W);
        return static_cast<std::size_t>(r) * W + c;
    };
    const double mid_lat = 37.4;

    // 1. Water pixel west of the coast → masked to NaN.
    {
        const std::size_t i = idx(mid_lat, -122.7);
        assert(std::isnan(result.min_az_buf[i]) && std::isnan(result.max_az_buf[i]));
        std::puts("PASS: ocean pixel masked to NaN");
    }

    // 2. Land pixel just east of the coast → visible, finite range.
    {
        const std::size_t i = idx(mid_lat, -122.48);
        assert(std::isfinite(result.min_az_buf[i]) &&
               std::isfinite(result.max_az_buf[i]));
        assert(result.min_az_buf[i] <= result.max_az_buf[i]);
        std::puts("PASS: near-coast land pixel has a finite azimuth range");
    }

    // 3. Far-inland land pixel → curvature hides the ocean horizon over flat
    //    terrain → never-visible sentinel (+inf, -inf), not NaN.
    {
        const std::size_t i = idx(mid_lat, -122.25);
        const float pos_inf = std::numeric_limits<float>::infinity();
        assert(result.min_az_buf[i] == pos_inf && result.max_az_buf[i] == -pos_inf);
        std::puts("PASS: far-inland flat land hits the never-visible sentinel");
    }

    fs::remove_all(dir);
    std::puts("ALL PASS");
    return 0;
}
