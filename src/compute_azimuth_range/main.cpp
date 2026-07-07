// compute_azimuth_range — for each cell in a bounding box, compute the exact
// set of azimuths from which the ocean horizon is visible at sunset. Writes a
// packed per-pixel azimuth bitmask (ADR-0013) as a bytes_per_pixel-band uint8
// GeoTIFF, with the azimuth window/step, bit_count, and format_version embedded
// as metadata tags. Each pixel's bits (LSB-first, per the BitLayout wire
// contract) encode the three display states downstream rendering needs:
//
//   all bytes zero            ocean or DEM-no-data — transparent (flag clear)
//   flag set, no vis bits     land that no swept azimuth can see — opaque black
//   flag set, vis bits set    land visible at azimuth_min_deg + i·step for each
//                             set bit i — date-dependent colouring
//
// The land/water distinction is resolved by querying the ocean mask for every
// cell (the flag bit at index bit_count); water transparency dominates any
// computed visibility.
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
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gdal_priv.h>

#include "BitLayout.h"
#include "BitmaskGeoTiffWriter.h"
#include "DEMTileLoader.h"
#include "OceanMaskRasterizer.h"
#include "OsmWaterPolygonSource.h"
#include "PipelineConfig.h"
#include "ProductionAdapters.h"
#include "StripProcessor.h"
#include "ThreadPool.h"
#include "WestEdgePreflight.h"

// Wire-format version stamped into the GeoTIFF metadata (ADR-0013). The encoder
// and frontend reject a packing they don't understand by checking this.
static constexpr int kFormatVersion = 1;

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

    GDALAllRegister();

    // ── Shared production loaders (one instance, reused per strip) ────────
    // Used serially: to freeze each strip's working set before its parallel
    // region, and for water-disambiguation after it.  Workers never touch these
    // directly — they read through the per-strip frozen views.
    DEMTileLoader       dem_loader(config.dem_dir,   config.dem_lru_capacity);
    OceanMaskRasterizer omr(
        std::make_unique<OsmWaterPolygonSource>(config.osm_water_polygons_path,
                                                config.osm_inland_water_path),
        config.ocean_lru_capacity);

    // ── West-edge preflight (ADR-0015) ─────────────────────────────────────
    // Every ray seeds at the western edge and marches east (ADR-0008), so a
    // box whose west edge is not offshore would sweep to plausible-looking
    // garbage that the encoder cannot detect. Hard-error before any output
    // exists; a small land fraction (config knob) tolerates a stray rock.
    {
        WaterAdapter water(omr);
        const WestEdgeCheck west = check_west_edge_offshore(
            water, min_lat, max_lat, min_lon, total_height,
            config.west_edge_max_land_frac);
        if (!west.offshore) {
            std::fprintf(stderr,
                "Error: the box's western edge (lon %.4f) is not offshore: "
                "%.1f%% of %d seed samples are land (max %.1f%%), "
                "land spans lat [%.4f, %.4f]. Nudge the western edge seaward "
                "of that range (ADR-0008).\n",
                min_lon, west.land_fraction * 100.0, west.total_samples,
                config.west_edge_max_land_frac * 100.0,
                west.land_min_lat, west.land_max_lat);
            return 1;
        }
    }

    // ── Pre-create the output GeoTIFF (north-up, EPSG:4326, packed uint8 mask) ─
    // The packing math (bit_count, bytes_per_pixel) comes from the single
    // BitLayout wire contract; the writer stamps the azimuth/format metadata.
    const BitLayout layout = BitLayout::from_config(
        config.azimuth_min_deg, config.azimuth_max_deg, config.azimuth_step_deg);

    std::unique_ptr<BitmaskGeoTiffWriter> writer;
    try {
        writer = std::make_unique<BitmaskGeoTiffWriter>(
            a.output_path, total_width, total_height,
            min_lon, max_lat, cell_deg, layout, kFormatVersion);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

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

        const int strip_h = result.height;

        // Place the strip at the correct rows of the output (slice row 0 =
        // south; GeoTIFF row 0 = north).  The writer owns the vertical flip and
        // the band de-interleave.
        const int y_off = total_height - rows_written_from_south - strip_h;
        if (y_off < 0) {
            std::fprintf(stderr,
                "Error: strip rows (%d so far + %d) exceed output height %d.\n",
                rows_written_from_south, strip_h, total_height);
            return 1;
        }

        try {
            writer->write_strip(result, y_off);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error: %s (strip %d).\n", e.what(), strip_index);
            return 1;
        }

        rows_written_from_south += strip_h;
        strip_min_lat = strip_max_lat;
        ++strip_index;
    }

    writer->close();

    std::printf("Wrote %s  (%d × %d pixels, %d strip%s)\n",
                a.output_path.c_str(), total_width, total_height,
                strip_index, strip_index == 1 ? "" : "s");
    return 0;
}
