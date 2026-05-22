#include "DEMTileLoader.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    // DEM_DATA_DIR has 8 USGS tiles — all must be indexed
    {
        DEMTileLoader loader(DEM_DATA_DIR);
        assert(loader.tile_count() == 8);
    }

    // A directory with no matching files produces an empty index without error
    {
        DEMTileLoader loader("/tmp");
        assert(loader.tile_count() == 0);
    }

    // A coordinate inside the indexed area returns a finite elevation
    {
        DEMTileLoader loader(DEM_DATA_DIR);
        float e = loader.get_elevation(37.5, -122.3);
        assert(std::isfinite(e));
    }

    // A coordinate outside the indexed area returns NaN
    {
        DEMTileLoader loader(DEM_DATA_DIR);
        float e = loader.get_elevation(35.5, -121.5);  // no tile for n36w122
        assert(std::isnan(e));
    }

    std::puts("PASS");
    return 0;
}
