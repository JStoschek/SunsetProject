#pragma once
#include <functional>

#include "HorizonSweepEngine.h"

// In-memory test fakes for the Horizon Sweep Engine (ADR-0009). They give exact
// analytic control of terrain and water with no files, so the visibility math
// can be unit-tested to floating-point precision.

/// Elevation from an analytic h(lat, lon) lambda.
struct FakeDEM : ElevationSource {
    std::function<float(double lat, double lon)> h;
    explicit FakeDEM(std::function<float(double, double)> fn) : h(std::move(fn)) {}
    float get_elevation(double lat, double lon) override { return h(lat, lon); }
};

/// A meridional (constant-longitude) coast: water strictly west of `coast_lon`,
/// land at and east of it. The engine's own march finds the crossing
/// (ADR-0014), so the fake only answers the point query.
struct FakeWater : WaterQuery {
    double coast_lon;
    explicit FakeWater(double lon) : coast_lon(lon) {}
    bool is_water(double, double lon) override { return lon < coast_lon; }
};
