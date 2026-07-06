#pragma once
#include "HorizonSweepEngine.h"
#include "DEMTileLoader.h"
#include "OceanMaskRasterizer.h"

// Thin adapters that wire the HorizonSweepEngine's abstract interfaces to the
// file-backed production loaders (ADR-0009). Neither loader class is modified.

struct DEMAdapter : ElevationSource {
    explicit DEMAdapter(DEMTileLoader& loader) : loader_(loader) {}
    float get_elevation(double lat, double lon) override {
        return loader_.get_elevation(lat, lon);
    }
private:
    DEMTileLoader& loader_;
};

// Point water query over the Ocean Mask. The engine folds coast-finding into
// its per-ray march (ADR-0014), so this is the only ocean access it needs.
struct WaterAdapter : WaterQuery {
    explicit WaterAdapter(OceanMaskRasterizer& omr) : omr_(omr) {}
    bool is_water(double lat, double lon) override {
        return omr_.is_water(lat, lon);
    }
private:
    OceanMaskRasterizer& omr_;
};
