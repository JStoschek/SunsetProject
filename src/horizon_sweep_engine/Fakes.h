#pragma once
#include <cmath>
#include <functional>

#include "HorizonSweepEngine.h"

// In-memory test fakes for the Horizon Sweep Engine (ADR-0009). They give exact
// analytic control of terrain and coast with no files, so the visibility math
// can be unit-tested to floating-point precision.

/// Elevation from an analytic h(lat, lon) lambda.
struct FakeDEM : ElevationSource {
    std::function<float(double lat, double lon)> h;
    explicit FakeDEM(std::function<float(double, double)> fn) : h(std::move(fn)) {}
    float get_elevation(double lat, double lon) override { return h(lat, lon); }
};

/// A meridional (constant-longitude) coast at a configured longitude. Marches
/// along `bearing` from the seed point to where the ray meets the coast, so the
/// returned crossing lies on the ray for any bearing — not just the cardinal
/// one. (At bearing 90 deg, cos b = 0 and the crossing latitude equals the seed
/// latitude, reducing to the trivial due-east case.)
struct FakeCoast : CoastlineFinder {
    double coast_lon;
    explicit FakeCoast(double lon) : coast_lon(lon) {}
    OceanOriginResult ocean_origin_for_ray(double bearing,
                                           double lat, double lon) override {
        constexpr double kPi = 3.14159265358979323846;
        const double b = bearing * kPi / 180.0;
        // Flat-earth march along the bearing: dlat/dlon = cos b / (sin b cos lat),
        // so the latitude gained reaching coast_lon is independent of mpd.
        const double crossing_lat =
            lat + (coast_lon - lon) * std::cos(lat * kPi / 180.0) *
                      std::cos(b) / std::sin(b);
        OceanOriginResult r;
        r.origin_lat = crossing_lat;  // vestigial (engine ignores the 200 km origin)
        r.origin_lon = coast_lon;     // "
        r.coast_lat  = crossing_lat;
        r.coast_lon  = coast_lon;
        return r;
    }
};
