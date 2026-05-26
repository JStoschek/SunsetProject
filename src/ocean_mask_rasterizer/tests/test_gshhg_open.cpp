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

    std::puts("PASS");
    return 0;
}
