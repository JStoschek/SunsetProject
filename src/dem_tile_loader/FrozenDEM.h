#pragma once
#include <climits>
#include <limits>

#include "DEMTile.h"
#include "FrozenTileSet.h"
#include "GeoTile.h"

// An immutable set of DEM tiles for one strip's working set.  Just a named
// FrozenTileSet of DEM tiles (built once by DEMTileLoader::freeze, read
// concurrently by many FrozenDEMView handles, never evicted) — the elevation
// cursor and no-data policy live in FrozenDEMView below.
class FrozenDEM : public FrozenTileSet<DEMTile> {};

// A per-worker, read-only handle onto a shared FrozenDEM.  Each worker owns its
// own view so the last-tile fast path (the cursor below) survives parallelism
// with no shared mutation and no locks on the get_elevation hot path.
class FrozenDEMView {
public:
    explicit FrozenDEMView(const FrozenDEM& frozen) : frozen_(frozen) {}

    float get_elevation(double lat, double lon) {
        const GeoTile key = GeoTile::owning(lat, lon);

        const DEMTile* tile;
        if (last_tile_ && key == last_key_) {
            tile = last_tile_;                       // fast path: same tile
        } else {
            tile = frozen_.find(key);
            if (!tile)
                return std::numeric_limits<float>::quiet_NaN();
            last_key_  = key;
            last_tile_ = tile;
        }
        return sample_elevation(*tile, lat, lon);
    }

private:
    const FrozenDEM& frozen_;
    GeoTile          last_key_{INT_MAX, INT_MAX};
    const DEMTile*   last_tile_ = nullptr;
};
