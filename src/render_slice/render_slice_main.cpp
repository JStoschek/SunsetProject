// render_slice — diagnostic tool that runs the Horizon Sweep Engine for a
// single sunset azimuth and writes a single-band GeoTIFF (1 = visible, 0 =
// not visible). Load it in QGIS or any GIS tool; overlay a basemap to see
// which pixels can see the sunset over the ocean horizon.
//
// Usage:
//   render_slice --config <pipeline.conf> \
//                --bbox <top_lat> <top_lon> <bot_lat> <bot_lon> \
//                --azimuth <degrees> \
//                [--output <path>]   # default: slice_<az>deg.tif
//
// The bounding box is given as top-left (north-west) followed by bottom-right
// (south-east): top_lat > bot_lat, top_lon < bot_lon (for western hemisphere).
// The western edge of the box must lie offshore the mainland so the coast
// finder has open water to start from (see ADR-0008).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include "ProductionAdapters.h"
#include "PipelineConfig.h"

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

struct Args {
    std::string config_path;
    double      top_lat   = 0, top_lon   = 0;
    double      bot_lat   = 0, bot_lon   = 0;
    double      azimuth   = 0;
    std::string output_path;
    bool        has_bbox  = false;
    bool        has_az    = false;
};

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --config <pipeline.conf>\n"
        "          --bbox <top_lat> <top_lon> <bot_lat> <bot_lon>\n"
        "          --azimuth <degrees>\n"
        "          [--output <path>]\n", argv0);
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
        } else if (arg == "--azimuth" && i + 1 < argc) {
            a.azimuth = std::stod(argv[++i]);
            a.has_az  = true;
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

    if (a.config_path.empty() || !a.has_bbox || !a.has_az) {
        usage(argv[0]);
        return 1;
    }

    // top-left / bottom-right → engine convention (min/max lat/lon)
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

    std::string output_path = a.output_path;
    if (output_path.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "slice_%.0fdeg.tif", a.azimuth);
        output_path = buf;
    }

    // ── Load config and wire up the engine ───────────────────────────────
    const PipelineConfig config = PipelineConfig::load(a.config_path);

    GDALAllRegister();

    DEMTileLoader       dem_loader(config.dem_dir,   config.dem_lru_capacity);
    OceanMaskRasterizer omr(config.gshhg_path,        config.ocean_lru_capacity);
    DEMAdapter          dem_adapter(dem_loader);
    OceanAdapter        ocean_adapter(omr, config);

    HorizonSweepEngine engine(dem_adapter, ocean_adapter, config,
                              min_lat, max_lat, min_lon, max_lon);

    std::printf("Computing azimuth slice at %.1f° over bbox "
                "[%.4f,%.4f] – [%.4f,%.4f]...\n",
                a.azimuth, min_lat, min_lon, max_lat, max_lon);

    AzimuthSlice slice;
    engine.compute_slice(a.azimuth, slice);

    // ── Write GeoTIFF ────────────────────────────────────────────────────
    // AzimuthSlice: row 0 = southernmost row.
    // GeoTIFF (north-up): row 0 = northernmost row. Flip vertically.

    const double cell_deg = 1.0 / config.cell_per_degree;

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!drv) {
        std::fprintf(stderr, "Error: GTiff GDAL driver not available.\n");
        return 1;
    }

    GDALDataset* ds = drv->Create(output_path.c_str(),
                                  slice.width, slice.height,
                                  1, GDT_Byte, nullptr);
    if (!ds) {
        std::fprintf(stderr, "Error: could not create '%s'.\n", output_path.c_str());
        return 1;
    }

    // GeoTransform: origin at top-left corner (min_lon, max_lat).
    double gt[6] = { min_lon, cell_deg, 0.0, max_lat, 0.0, -cell_deg };
    ds->SetGeoTransform(gt);

    // WGS 84 spatial reference.
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    ds->SetProjection(wkt);
    CPLFree(wkt);

    GDALRasterBand* band = ds->GetRasterBand(1);

    std::vector<uint8_t> row_buf(static_cast<std::size_t>(slice.width));
    for (int tiff_row = 0; tiff_row < slice.height; ++tiff_row) {
        // tiff_row 0 (north) corresponds to slice row (height - 1) (south = 0).
        const int slice_row = slice.height - 1 - tiff_row;
        for (int col = 0; col < slice.width; ++col) {
            row_buf[col] = slice.visible[
                static_cast<std::size_t>(slice_row) * slice.width + col] ? 1 : 0;
        }
        const CPLErr err = band->RasterIO(GF_Write, 0, tiff_row, slice.width, 1,
                                          row_buf.data(), slice.width, 1, GDT_Byte, 0, 0);
        if (err != CE_None) {
            std::fprintf(stderr, "Error: RasterIO write failed at row %d.\n", tiff_row);
            GDALClose(ds);
            return 1;
        }
    }

    GDALClose(ds);

    std::printf("Wrote %s  (%d × %d pixels)\n",
                output_path.c_str(), slice.width, slice.height);
    return 0;
}
