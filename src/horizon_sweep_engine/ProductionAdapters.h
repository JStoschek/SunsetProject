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

// `bearing` is the ocean→land march bearing = (sunset_az + 180) mod 360.
// Delegates directly to OceanMaskRasterizer::ocean_origin_for_ray, which
// accepts the same convention.  Only the coastline crossing fields are used
// by the engine; the 200 km origin fields are ignored.
struct OceanAdapter : CoastlineFinder {
    explicit OceanAdapter(OceanMaskRasterizer& omr) : omr_(omr) {}
    OceanOriginResult ocean_origin_for_ray(double bearing,
                                           double lat, double lon) override {
        return omr_.ocean_origin_for_ray(bearing, lat, lon);
    }
private:
    OceanMaskRasterizer& omr_;
};
