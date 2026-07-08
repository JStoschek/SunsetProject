#include "DEMTileLoader.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void touch(const fs::path& p) { std::ofstream f(p); }

int main() {
    // Discovery only parses filenames, so empty .tif files make a fixture —
    // unlike DEM_DATA_DIR, its contents can't drift as real tiles are added.
    const fs::path dir = fs::temp_directory_path() / "tile_discovery_fixture";
    fs::remove_all(dir);
    fs::create_directories(dir / "empty");
    touch(dir / "USGS_13_n37w122_20100929.tif");
    touch(dir / "USGS_13_n38w122_20100929.tif");
    touch(dir / "USGS_13_n38w122_20250826.tif");  // same tile, newer acquisition
    touch(dir / "USGS_13_n39w123_20250520.tif");
    touch(dir / "USGS_13_n39w123_20250520_v2.tif");  // same tile, same date
    touch(dir / "README.txt");                        // non-matching: ignored

    // Duplicate tile ids collapse to one index entry per GeoTile
    {
        DEMTileLoader loader(dir.string());
        assert(loader.tile_count() == 3);

        // The newest acquisition date wins for a duplicated tile
        const std::string* p = loader.tile_path(GeoTile::from_usgs(38, 122));
        assert(p && fs::path(*p).filename() == "USGS_13_n38w122_20250826.tif");

        // Equal dates tie-break on the lexicographically greatest filename,
        // so the choice never depends on directory-iteration order
        p = loader.tile_path(GeoTile::from_usgs(39, 123));
        assert(p && fs::path(*p).filename() == "USGS_13_n39w123_20250520_v2.tif");

        // An uncovered tile has no path
        assert(loader.tile_path(GeoTile::from_usgs(10, 10)) == nullptr);
    }

    // A directory with no matching files produces an empty index without error
    {
        DEMTileLoader loader((dir / "empty").string());
        assert(loader.tile_count() == 0);
    }

    // Real data: a coordinate inside the indexed area returns a finite elevation
    {
        DEMTileLoader loader(DEM_DATA_DIR);
        assert(loader.tile_count() > 0);
        float e = loader.get_elevation(37.5, -122.3);
        assert(std::isfinite(e));
    }

    // A coordinate with no tile indexed returns NaN (fixture has no n36w122,
    // and the miss is answered from the index without opening any file)
    {
        DEMTileLoader loader(dir.string());
        float e = loader.get_elevation(35.5, -121.5);
        assert(std::isnan(e));
    }

    fs::remove_all(dir);
    std::puts("PASS");
    return 0;
}
