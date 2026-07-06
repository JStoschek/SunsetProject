#include "StripProcessor.h"

#include <set>

#include "DEMTileLoader.h"
#include "FrozenStripSources.h"
#include "GeoTile.h"
#include "OceanMaskRasterizer.h"
#include "StripPostProcess.h"
#include "StripWorkingSet.h"

StripProcessor::StripProcessor(DEMTileLoader&        dem,
                               OceanMaskRasterizer&  omr,
                               ThreadPool&           pool,
                               const PipelineConfig& config,
                               double min_lon, double max_lon)
    : dem_(dem), omr_(omr), pool_(pool), config_(config),
      min_lon_(min_lon), max_lon_(max_lon) {}

StripResult StripProcessor::process(double strip_min_lat, double strip_max_lat) {
    // Freeze the strip's working set (bbox + ray-tilt margin) into eviction-free
    // per-strip caches.  These locals outlive the parallel region below and die
    // with this call — the next strip gets its own.
    const std::set<GeoTile> work = strip_working_set(
        strip_min_lat, strip_max_lat, min_lon_, max_lon_,
        config_.strip_tilt_margin_deg);
    const FrozenDEM   frozen_dem   = dem_.freeze(work);
    const FrozenOcean frozen_ocean = omr_.freeze(work);
    FrozenStripSources sources(frozen_dem, frozen_ocean, pool_.worker_count());

    // Lock-free parallel azimuth sweep over the frozen caches.
    StripResult result = sweep_strip(sources, config_, pool_,
                                     strip_min_lat, strip_max_lat,
                                     min_lon_, max_lon_);

    // Set the land/data flag: water pixels are zeroed (transparent), all other
    // land pixels get the flag bit — including land that earned no visible
    // azimuth (opaque black).  Uses the stateful rasterizer directly (serial
    // phase — workers have finished).
    const BitLayout layout = BitLayout::from_config(
        config_.azimuth_min_deg, config_.azimuth_max_deg, config_.azimuth_step_deg);
    const double cell_deg = 1.0 / config_.cell_per_degree;
    apply_land_data_flag(result, layout, strip_min_lat, min_lon_, cell_deg,
                         [this](double lat, double lon) {
                             return omr_.is_water(lat, lon);
                         });
    return result;
}
