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
#include <stdexcept>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include "AzimuthRangeAccumulator.h"
#include "HorizonSweepEngine.h"
#include "PipelineConfig.h"
#include "ProductionAdapters.h"

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

    std::printf("compute_azimuth_range: bbox [%.4f,%.4f]–[%.4f,%.4f]"
                "  azimuths %.1f–%.1f step %.1f  output %s\n",
                min_lat, min_lon, max_lat, max_lon,
                config.azimuth_min_deg, config.azimuth_max_deg,
                config.azimuth_step_deg, a.output_path.c_str());
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

    // ── Shared production adapters (one instance, reused per strip) ──────
    DEMTileLoader       dem_loader(config.dem_dir,   config.dem_lru_capacity);
    OceanMaskRasterizer omr(config.gshhg_path,        config.ocean_lru_capacity);
    DEMAdapter          dem_adapter(dem_loader);
    OceanAdapter        ocean_adapter(omr, config);

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

        HorizonSweepEngine engine(dem_adapter, ocean_adapter, config,
                                  strip_min_lat, strip_max_lat,
                                  min_lon, max_lon);

        AzimuthSlice slice;
        // Run one slice up-front so we can size the accumulator from the
        // engine's reported dimensions for this strip.
        int az_idx = 1;
        std::printf("  strip %d/%d  lat [%.4f, %.4f]  az %.0f°  [%d/%d]  \r",
                    strip_index + 1, total_strips,
                    strip_min_lat, strip_max_lat,
                    config.azimuth_min_deg, az_idx, az_count);
        std::fflush(stdout);
        engine.compute_slice(config.azimuth_min_deg, slice);

        const int strip_w = slice.width;
        const int strip_h = slice.height;
        const std::size_t n_pix =
            static_cast<std::size_t>(strip_w) * strip_h;

        AzimuthRangeAccumulator acc(n_pix);
        acc.accumulate(slice, config.azimuth_min_deg);

        for (double az = config.azimuth_min_deg + config.azimuth_step_deg;
             az <= config.azimuth_max_deg + 0.5 * config.azimuth_step_deg;
             az += config.azimuth_step_deg) {
            ++az_idx;
            std::printf("  strip %d/%d  lat [%.4f, %.4f]  az %.0f°  [%d/%d]  \r",
                        strip_index + 1, total_strips,
                        strip_min_lat, strip_max_lat,
                        az, az_idx, az_count);
            std::fflush(stdout);
            engine.compute_slice(az, slice);
            acc.accumulate(slice, az);
        }
        std::printf("  strip %d/%d  lat [%.4f, %.4f]  done%*s\n",
                    strip_index + 1, total_strips,
                    strip_min_lat, strip_max_lat, 16, "");
        std::fflush(stdout);

        // Disambiguate ocean-or-no-data from never-visible-land. The sweep
        // leaves both as NaN; we query the GSHHG ocean mask and rewrite the
        // land NaNs to (+inf, -inf) so the encoder can render them opaque.
        const float pos_inf = std::numeric_limits<float>::infinity();
        const float neg_inf = -std::numeric_limits<float>::infinity();
        for (int r = 0; r < strip_h; ++r) {
            const double lat_pix = strip_min_lat + r * cell_deg;
            for (int c = 0; c < strip_w; ++c) {
                const std::size_t i =
                    static_cast<std::size_t>(r) * strip_w + c;
                if (!std::isnan(acc.min_az_buf[i])) continue;
                const double lon_pix = min_lon + c * cell_deg;
                if (!omr.is_water(lat_pix, lon_pix)) {
                    acc.min_az_buf[i] = pos_inf;
                    acc.max_az_buf[i] = neg_inf;
                }
            }
        }

        // Flip each strip vertically (slice row 0 = south; GeoTIFF row 0 =
        // north) and write the block to the correct rows of the output.
        // Global GeoTIFF Y-offset for the top (north) edge of this strip:
        //   y_off = total_height - rows_written_from_south - strip_h
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
                (band_idx == 0) ? acc.min_az_buf : acc.max_az_buf;
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
