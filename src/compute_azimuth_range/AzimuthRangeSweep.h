#pragma once
#include <vector>

#include "HorizonSweepEngine.h"
#include "PipelineConfig.h"
#include "ThreadPool.h"

/// Accumulated min/max azimuth buffers for one latitude strip.
struct StripResult {
    std::vector<float> min_az_buf;
    std::vector<float> max_az_buf;
    int width  = 0;
    int height = 0;
};

/// Supplies each pool worker with its own elevation/coast abstractions for a
/// strip.  Production hands out per-worker FrozenDEMView / FrozenOceanView
/// (lock-free reads over the strip's frozen working set — see FrozenStripSources);
/// tests hand out a shared stateless fake.  `worker` is in [0, worker_count).
/// The returned references must outlive the sweep_strip call.
struct StripSources {
    virtual ~StripSources() = default;
    virtual ElevationSource& dem_for_worker(int worker)   = 0;
    virtual CoastlineFinder& ocean_for_worker(int worker) = 0;
};

/// Run the full azimuth sweep for one latitude strip and return the per-pixel
/// min/max azimuth range.  Dispatches each azimuth-slice computation to `pool`
/// (one task per worker, each claiming azimuths from a shared counter).  Each
/// worker reads through its own sources (see StripSources), so the hot
/// get_elevation / coast-finding paths carry no locks or shared mutation.
StripResult sweep_strip(StripSources&         sources,
                        const PipelineConfig& config,
                        ThreadPool&           pool,
                        double strip_min_lat, double strip_max_lat,
                        double min_lon,       double max_lon);
