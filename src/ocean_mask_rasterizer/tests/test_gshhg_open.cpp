#include "OceanMaskRasterizer.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>

int main() {
    // Constructing against the real gshhs_f.b must succeed without throwing or crashing.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        (void)omr;
    }

    // Stub: is_water always returns false
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        assert(omr.is_water(37.5, -122.3) == false);
    }

    // Stub: ocean_origin_for_ray returns {lat, lon} unchanged
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto [lat, lon] = omr.ocean_origin_for_ray(270.0, 37.5, -122.3);
        assert(lat == 37.5);
        assert(lon == -122.3);
    }

    std::puts("PASS");
    return 0;
}
