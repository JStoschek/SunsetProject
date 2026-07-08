// trace_ray — diagnostic single-ray trace through the *real* HorizonSweepEngine.
//
// Wires the production DEM + OSM-ocean loaders to the engine over a tiny bbox
// around a target point, enables the engine's built-in RayTrace, and runs one
// azimuth slice. The engine then prints, for the single parallel ray that
// carries the target pixel, every march sample — elevation, is_water, the
// running max Horizon Reach, the observer's reach (ADR-0016), and the STORED
// visibility verdict — from `--before` samples seaward of the coastline
// crossing through `--after` samples inland (ADR-0014: a ray is a plain list
// of yes/no points).
//
// This is a debugging tool: it does NOT re-derive the math, it observes what the
// engine actually computes (the recorded values come straight out of the live
// compute_slice run).
//
// Usage:
//   trace_ray --config <pipeline.conf> --lat <deg> --lon <deg>
//             --azimuth <sunset_deg> [--before N] [--after N]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include <gdal_priv.h>

#include "DEMTileLoader.h"
#include "HorizonSweepEngine.h"
#include "OceanMaskRasterizer.h"
#include "OsmWaterPolygonSource.h"
#include "PipelineConfig.h"
#include "ProductionAdapters.h"

int main(int argc, char* argv[]) {
    std::string config_path;
    double target_lat = 0, target_lon = 0, azimuth = 0;
    int    before = 100, after = 100;
    bool   have_lat = false, have_lon = false, have_az = false;
    // Optional explicit bbox override (to reproduce the production extent and
    // show how the coast crossing — hence visibility — shifts with the box).
    double ov_min_lat = 0, ov_max_lat = 0, ov_min_lon = 0, ov_max_lon = 0;
    bool   have_box = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (a == "--config")  config_path = next("--config");
        else if (a == "--lat")   { target_lat = std::stod(next("--lat")); have_lat = true; }
        else if (a == "--lon")   { target_lon = std::stod(next("--lon")); have_lon = true; }
        else if (a == "--azimuth") { azimuth = std::stod(next("--azimuth")); have_az = true; }
        else if (a == "--before")  before = std::stoi(next("--before"));
        else if (a == "--after")   after  = std::stoi(next("--after"));
        else if (a == "--bbox") {  // top_lat top_lon bot_lat bot_lon (NW then SE)
            ov_max_lat = std::stod(next("--bbox"));
            ov_min_lon = std::stod(next("--bbox"));
            ov_min_lat = std::stod(next("--bbox"));
            ov_max_lon = std::stod(next("--bbox"));
            have_box = true;
        }
        else { std::fprintf(stderr, "Unknown argument: %s\n", a.c_str()); return 1; }
    }

    if (config_path.empty() || !have_lat || !have_lon || !have_az) {
        std::fprintf(stderr,
            "Usage: %s --config <pipeline.conf> --lat <deg> --lon <deg>\n"
            "          --azimuth <sunset_deg> [--before N] [--after N]\n",
            argv[0]);
        return 1;
    }

    GDALAllRegister();

    PipelineConfig config;
    try {
        config = PipelineConfig::load(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error loading config: %s\n", e.what());
        return 1;
    }

    // A small bbox around the target. The western edge must sit offshore so the
    // coast march (ocean->land, bearing = az+180) starts in open water; the box
    // is padded generously to the west and north because for NW sunset azimuths
    // the ray enters the box from the north-west. The lat extent is kept tight
    // to keep the run fast — only the one ray through the target is dumped.
    const double min_lon = have_box ? ov_min_lon : target_lon - 0.035;
    const double max_lon = have_box ? ov_max_lon : target_lon + 0.010;
    const double min_lat = have_box ? ov_min_lat : target_lat - 0.010;
    const double max_lat = have_box ? ov_max_lat : target_lat + 0.035;

    DEMTileLoader       dem_loader(config.dem_dir, config.dem_lru_capacity);
    OceanMaskRasterizer omr(
        std::make_unique<OsmWaterPolygonSource>(config.osm_water_polygons_path,
                                                config.osm_inland_water_path),
        config.ocean_lru_capacity);

    DEMAdapter   dem_adapter(dem_loader);
    WaterAdapter water_adapter(omr);

    HorizonSweepEngine engine(dem_adapter, water_adapter, config,
                              min_lat, max_lat, min_lon, max_lon);

    HorizonSweepEngine::RayTrace trace;
    trace.enabled      = true;
    trace.target_lat   = target_lat;
    trace.target_lon   = target_lon;
    trace.steps_before = before;
    trace.steps_after  = after;
    trace.water_query  = [&omr](double lat, double lon) {
        return omr.is_water(lat, lon);
    };
    engine.set_trace(trace);

    AzimuthSlice slice;
    engine.compute_slice(azimuth, slice);

    // Report the engine's final verdict for the exact target pixel (gathered
    // from its nearest sample).
    const double cell_deg = 1.0 / config.cell_per_degree;
    const int col = static_cast<int>(std::lround((target_lon - min_lon) / cell_deg));
    const int row = static_cast<int>(std::lround((target_lat - min_lat) / cell_deg));
    if (col >= 0 && col < slice.width && row >= 0 && row < slice.height) {
        const bool vis =
            slice.visible[static_cast<std::size_t>(row) * slice.width + col];
        std::printf("Gathered verdict for target pixel (row %d, col %d): %s\n",
                    row, col, vis ? "VISIBLE" : "NOT visible");
    } else {
        std::printf("Target pixel fell outside the trace bbox grid.\n");
    }
    return 0;
}
