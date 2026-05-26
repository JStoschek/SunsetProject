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

    // ocean_origin_for_ray is still a stub: returns {lat, lon} unchanged.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto [lat, lon] = omr.ocean_origin_for_ray(270.0, 37.5, -122.3);
        assert(lat == 37.5);
        assert(lon == -122.3);
    }

    std::puts("PASS");
    return 0;
}
