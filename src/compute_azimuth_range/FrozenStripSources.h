#pragma once
#include <vector>

#include "AzimuthRangeSweep.h"   // StripSources
#include "FrozenDEM.h"           // FrozenDEM / FrozenDEMView (dem_tile_loader)
#include "FrozenOcean.h"         // FrozenOcean / FrozenOceanView (ocean_mask_rasterizer)
#include "HorizonSweepEngine.h"  // ElevationSource / WaterQuery

// Production StripSources backed by a strip's frozen caches.  Owns one
// FrozenDEMView and one FrozenOceanView per worker, each with its own cursor
// pointing at the shared, immutable frozen tile data — so the get_elevation /
// is_water hot paths run with no locks and no shared mutation.
//
// The referenced FrozenDEM / FrozenOcean must outlive this object (they are
// built per strip and live for the duration of the parallel region).
class FrozenStripSources : public StripSources {
public:
    FrozenStripSources(const FrozenDEM&   dem,
                       const FrozenOcean& ocean,
                       int                worker_count) {
        dem_.reserve(worker_count);
        water_.reserve(worker_count);
        for (int i = 0; i < worker_count; ++i) {
            dem_.emplace_back(dem);
            water_.emplace_back(ocean);
        }
    }

    ElevationSource& dem_for_worker(int worker) override   { return dem_[worker]; }
    WaterQuery&      water_for_worker(int worker) override { return water_[worker]; }

private:
    // Per-worker elevation source wrapping a frozen view.
    struct DEMSource : ElevationSource {
        FrozenDEMView view;
        explicit DEMSource(const FrozenDEM& f) : view(f) {}
        float get_elevation(double lat, double lon) override {
            return view.get_elevation(lat, lon);
        }
    };

    // Per-worker water query wrapping a frozen view.  The engine's own march
    // finds the coast (ADR-0014), so only the point query is exposed.
    struct WaterSource : WaterQuery {
        FrozenOceanView view;
        explicit WaterSource(const FrozenOcean& f) : view(f) {}
        bool is_water(double lat, double lon) override {
            return view.is_water(lat, lon);
        }
    };

    std::vector<DEMSource>   dem_;
    std::vector<WaterSource> water_;
};
