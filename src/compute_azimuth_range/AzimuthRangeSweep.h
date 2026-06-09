#pragma once
#include <cstdint>
#include <vector>

#include "HorizonSweepEngine.h"
#include "PipelineConfig.h"
#include "ThreadPool.h"

/// Packed per-pixel azimuth bitmask for one latitude strip (ADR-0013).  `mask`
/// is width*height*bytes_per_pixel bytes, row 0 = south, each pixel's bits
/// packed LSB-first per the BitLayout wire contract: bit i = visible at the
/// i-th swept azimuth, bit bit_count = the land/data flag.
struct StripResult {
    std::vector<std::uint8_t> mask;
    int width           = 0;
    int height          = 0;
    int bytes_per_pixel = 0;
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
