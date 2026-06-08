// compute_azimuth_range — for each cell in a bounding box, compute the range
// of azimuths from which the ocean horizon is visible at sunset. Writes a
// two-band Float32 GeoTIFF (band 1 = min_az, band 2 = max_az) with a
// three-state encoding so downstream rendering can distinguish ocean from
// land that is permanently behind a ridge:
//
//   (NaN,  NaN)  ocean or DEM-no-data — no measurement applies
//   (+inf, -inf) land cell whose visible-azimuth range is empty (every
//                swept azimuth was blocked by terrain)
//   (min,  max)  land cell, visible at least once in [azimuth_min_deg,
//                azimuth_max_deg]; min ≤ max, both finite
//
// The land/empty-range distinction is resolved by querying the ocean mask
// (GSHHG) for every cell where the sweep produced no visible azimuth.
//
// Usage:
//   compute_azimuth_range --config <pipeline.conf>
//                         --bbox <top_lat> <top_lon> <bot_lat> <bot_lon>
//                         --output <path>
//
// The bounding box is given as top-left (north-west) followed by bottom-right
// (south-east): top_lat > bot_lat, top_lon < bot_lon.

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include "DEMTileLoader.h"
#include "OceanMaskRasterizer.h"
#include "OsmWaterPolygonSource.h"
#include "PipelineConfig.h"
#include "StripProcessor.h"
#include "ThreadPool.h"

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

struct Args {
    std::string config_path;
    double      top_lat  = 0, top_lon  = 0;
    double      bot_lat  = 0, bot_lon  = 0;
    std::string output_path;
    bool        has_bbox = false;
};

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --config <pipeline.conf>\n"
        "          --bbox <top_lat> <top_lon> <bot_lat> <bot_lon>\n"
        "          --output <path>\n", argv0);
}

static Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            a.config_path = argv[++i];
        } else if (arg == "--bbox" && i + 4 < argc) {
            a.top_lat  = std::stod(argv[++i]);
            a.top_lon  = std::stod(argv[++i]);
            a.bot_lat  = std::stod(argv[++i]);
            a.bot_lon  = std::stod(argv[++i]);
            a.has_bbox = true;
        } else if (arg == "--output" && i + 1 < argc) {
            a.output_path = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const Args a = parse_args(argc, argv);

    if (a.config_path.empty() || !a.has_bbox || a.output_path.empty()) {
        usage(argv[0]);
        return 1;
    }

    const double min_lat = a.bot_lat;
    const double max_lat = a.top_lat;
    const double min_lon = a.top_lon;
    const double max_lon = a.bot_lon;

    if (min_lat >= max_lat || min_lon >= max_lon) {
        std::fprintf(stderr,
            "Error: bounding box is degenerate — "
            "top_lat must exceed bot_lat and bot_lon must exceed top_lon.\n");
        return 1;
    }

    PipelineConfig config;
    try {
        config = PipelineConfig::load(a.config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    const double cell_deg = 1.0 / config.cell_per_degree;
    const int total_width  =
        static_cast<int>(std::lround((max_lon - min_lon) * config.cell_per_degree));
    const int total_height =
        static_cast<int>(std::lround((max_lat - min_lat) * config.cell_per_degree));

    if (total_width <= 0 || total_height <= 0) {
        std::fprintf(stderr, "Error: bounding box resolves to zero pixels.\n");
        return 1;
    }

    // Resolve worker_threads = 0 to all hardware cores.
    const int nw = (config.worker_threads > 0)
                 ? config.worker_threads
                 : static_cast<int>(std::thread::hardware_concurrency());

    std::printf("compute_azimuth_range: bbox [%.4f,%.4f]–[%.4f,%.4f]"
                "  azimuths %.1f–%.1f step %.1f  workers %d  output %s\n",
                min_lat, min_lon, max_lat, max_lon,
                config.azimuth_min_deg, config.azimuth_max_deg,
                config.azimuth_step_deg, nw, a.output_path.c_str());
    std::printf("Output grid: %d × %d pixels (cell = %.6f°)\n",
                total_width, total_height, cell_deg);

    // ── Pre-create the output GeoTIFF (north-up, EPSG:4326, 2 Float32 bands) ──
    GDALAllRegister();

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!drv) {
        std::fprintf(stderr, "Error: GTiff GDAL driver not available.\n");
        return 1;
    }

    const float nan_f = std::numeric_limits<float>::quiet_NaN();

    char** create_opts = nullptr;
    create_opts = CSLSetNameValue(create_opts, "COMPRESS", "DEFLATE");
    create_opts = CSLSetNameValue(create_opts, "TILED",    "YES");

    GDALDataset* ds = drv->Create(a.output_path.c_str(),
                                  total_width, total_height,
                                  2, GDT_Float32, create_opts);
    CSLDestroy(create_opts);
    if (!ds) {
        std::fprintf(stderr, "Error: could not create '%s'.\n",
                     a.output_path.c_str());
        return 1;
    }

    double gt[6] = { min_lon, cell_deg, 0.0, max_lat, 0.0, -cell_deg };
    ds->SetGeoTransform(gt);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    ds->SetProjection(wkt);
    CPLFree(wkt);

    GDALRasterBand* band_min = ds->GetRasterBand(1);
    GDALRasterBand* band_max = ds->GetRasterBand(2);
    band_min->SetNoDataValue(nan_f);
    band_max->SetNoDataValue(nan_f);
    band_min->SetDescription("min_az_deg");
    band_max->SetDescription("max_az_deg");

    // ── Shared production loaders (one instance, reused per strip) ────────
    // Used serially: to freeze each strip's working set before its parallel
    // region, and for water-disambiguation after it.  Workers never touch these
    // directly — they read through the per-strip frozen views.
    DEMTileLoader       dem_loader(config.dem_dir,   config.dem_lru_capacity);
    OceanMaskRasterizer omr(
        std::make_unique<OsmWaterPolygonSource>(config.osm_water_polygons_path,
                                                config.osm_inland_water_path),
        config.ocean_lru_capacity);

    // ── Thread pool: created once, reused across all strips ───────────────
    config.worker_threads = nw;  // store resolved count for sweep_strip
    ThreadPool pool(nw);

    // ── Per-strip orchestration: freeze → sweep → water-mask ──────────────
    // Owns the freeze-before-parallel structure (ADR-0011); main keeps the
    // GeoTIFF I/O below.
    StripProcessor strip_processor(dem_loader, omr, pool, config,
                                   min_lon, max_lon);

    // ── Strip loop (south → north) ───────────────────────────────────────
    if (config.strip_height_deg <= 0.0) {
        std::fprintf(stderr, "Error: strip_height_deg must be positive.\n");
        GDALClose(ds);
        return 1;
    }

    const int total_strips = static_cast<int>(
        std::ceil((max_lat - min_lat) / config.strip_height_deg));
    const int az_count = static_cast<int>(
        std::lround((config.azimuth_max_deg - config.azimuth_min_deg)
                    / config.azimuth_step_deg)) + 1;

    int strip_index = 0;
    int rows_written_from_south = 0;
    double strip_min_lat = min_lat;

    while (strip_min_lat < max_lat) {
        double strip_max_lat = strip_min_lat + config.strip_height_deg;
        if (strip_max_lat > max_lat) strip_max_lat = max_lat;

        std::printf("  strip %d/%d  lat [%.4f, %.4f]  %d azimuths  workers %d\n",
                    strip_index + 1, total_strips,
                    strip_min_lat, strip_max_lat, az_count, nw);
        std::fflush(stdout);

        // Freeze the strip's working set, sweep all azimuths in parallel, and
        // apply the water mask — all behind the StripProcessor seam.
        StripResult result =
            strip_processor.process(strip_min_lat, strip_max_lat);

        const int strip_w = result.width;
        const int strip_h = result.height;
        const std::size_t n_pix =
            static_cast<std::size_t>(strip_w) * strip_h;

        // Flip each strip vertically (slice row 0 = south; GeoTIFF row 0 =
        // north) and write the block to the correct rows of the output.
        const int y_off = total_height - rows_written_from_south - strip_h;
        if (y_off < 0) {
            std::fprintf(stderr,
                "Error: strip rows (%d so far + %d) exceed output height %d.\n",
                rows_written_from_south, strip_h, total_height);
            GDALClose(ds);
            return 1;
        }

        std::vector<float> flipped(n_pix);
        for (int band_idx = 0; band_idx < 2; ++band_idx) {
            const std::vector<float>& src =
                (band_idx == 0) ? result.min_az_buf : result.max_az_buf;
            for (int r = 0; r < strip_h; ++r) {
                const int tiff_row_in_strip = strip_h - 1 - r;
                std::copy_n(
                    src.data() + static_cast<std::size_t>(r) * strip_w,
                    strip_w,
                    flipped.data() +
                        static_cast<std::size_t>(tiff_row_in_strip) * strip_w);
            }
            GDALRasterBand* band =
                (band_idx == 0) ? band_min : band_max;
            const CPLErr err = band->RasterIO(GF_Write,
                                              0, y_off,
                                              strip_w, strip_h,
                                              flipped.data(),
                                              strip_w, strip_h,
                                              GDT_Float32, 0, 0);
            if (err != CE_None) {
                std::fprintf(stderr,
                    "Error: RasterIO write failed for strip %d band %d.\n",
                    strip_index, band_idx + 1);
                GDALClose(ds);
                return 1;
            }
        }

        rows_written_from_south += strip_h;
        strip_min_lat = strip_max_lat;
        ++strip_index;
    }

    GDALClose(ds);

    std::printf("Wrote %s  (%d × %d pixels, %d strip%s)\n",
                a.output_path.c_str(), total_width, total_height,
                strip_index, strip_index == 1 ? "" : "s");
    return 0;
}
