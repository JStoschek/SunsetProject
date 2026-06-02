#pragma once
#include <vector>

#include "AzimuthRangeSweep.h"   // StripSources
#include "FrozenDEM.h"           // FrozenDEM / FrozenDEMView (dem_tile_loader)
#include "FrozenOcean.h"         // FrozenOcean / FrozenOceanView (ocean_mask_rasterizer)
#include "HorizonSweepEngine.h"  // ElevationSource / CoastlineFinder
#include "PipelineConfig.h"

// Production StripSources backed by a strip's frozen caches.  Owns one
// FrozenDEMView and one FrozenOceanView per worker, each with its own cursor
// pointing at the shared, immutable frozen tile data — so the get_elevation /
// coast-finding hot paths run with no locks and no shared mutation.
//
// The referenced FrozenDEM / FrozenOcean must outlive this object (they are
// built per strip and live for the duration of the parallel region).
class FrozenStripSources : public StripSources {
public:
    FrozenStripSources(const FrozenDEM&      dem,
                       const FrozenOcean&    ocean,
                       const PipelineConfig& config,
                       int                   worker_count)
        : config_(config) {
        dem_.reserve(worker_count);
        ocean_.reserve(worker_count);
        for (int i = 0; i < worker_count; ++i) {
            dem_.emplace_back(dem);
            ocean_.emplace_back(ocean, config);
        }
    }

    ElevationSource& dem_for_worker(int worker) override   { return dem_[worker]; }
    CoastlineFinder& ocean_for_worker(int worker) override { return ocean_[worker]; }

private:
    // Per-worker elevation source wrapping a frozen view.
    struct DEMSource : ElevationSource {
        FrozenDEMView view;
        explicit DEMSource(const FrozenDEM& f) : view(f) {}
        float get_elevation(double lat, double lon) override {
            return view.get_elevation(lat, lon);
        }
    };

    // Per-worker coast finder wrapping a frozen view; applies the configured
    // march step / give-up distance (matching ProductionAdapters' OceanAdapter).
    struct OceanSource : CoastlineFinder {
        FrozenOceanView       view;
        const PipelineConfig* config;
        OceanSource(const FrozenOcean& f, const PipelineConfig& c)
            : view(f), config(&c) {}
        OceanOriginResult ocean_origin_for_ray(double bearing,
                                               double lat, double lon) override {
            return view.ocean_origin_for_ray(bearing, lat, lon,
                                             config->coast_march_step_km,
                                             config->coast_march_max_km);
        }
    };

    const PipelineConfig&   config_;
    std::vector<DEMSource>  dem_;
    std::vector<OceanSource> ocean_;
};
