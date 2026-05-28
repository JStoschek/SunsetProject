// compute_azimuth_range — for each cell in a bounding box, compute the range
// of azimuths from which the ocean horizon is visible at sunset.
//
// Usage:
//   compute_azimuth_range --config <pipeline.conf>
//                         --bbox <top_lat> <top_lon> <bot_lat> <bot_lon>
//                         --output <path>
//
// The bounding box is given as top-left (north-west) followed by bottom-right
// (south-east): top_lat > bot_lat, top_lon < bot_lon.

#include <cstdio>
#include <stdexcept>
#include <string>

#include "PipelineConfig.h"

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

    std::printf("compute_azimuth_range: bbox [%.4f,%.4f]–[%.4f,%.4f]"
                "  azimuths %.1f–%.1f step %.1f  output %s\n",
                min_lat, min_lon, max_lat, max_lon,
                config.azimuth_min_deg, config.azimuth_max_deg,
                config.azimuth_step_deg, a.output_path.c_str());
    return 0;
}
