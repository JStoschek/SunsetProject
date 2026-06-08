#pragma once
#include <climits>
#include <limits>
#include <memory>
#include <unordered_map>

#include "DEMTile.h"
#include "GeoTile.h"

// An immutable set of DEM tiles, sized to exactly one strip's working set.
// Built once (serially) by DEMTileLoader::freeze before a strip's parallel
// region and read concurrently by many FrozenDEMView handles.  It performs no
// eviction: every tile in the working set stays resident for the whole strip,
// so a worker can never read a tile another worker freed.
class FrozenDEM {
public:
    // Returns the tile owning `key`, or nullptr if it was not frozen (e.g. a
    // coordinate outside the working set — treated as no-data).
    const DEMTile* find(GeoTile key) const {
        auto it = tiles_.find(key);
        return it == tiles_.end() ? nullptr : it->second.get();
    }

    // Used by DEMTileLoader::freeze to populate the structure.  Tile buffers are
    // shared (shared_ptr) so freezing a tile already resident in the loader's
    // cache costs no reload or copy, and adjacent strips reuse the same data.
    void insert(GeoTile key, std::shared_ptr<const DEMTile> tile) {
        tiles_.emplace(key, std::move(tile));
    }

    std::size_t tile_count() const { return tiles_.size(); }

private:
    std::unordered_map<GeoTile, std::shared_ptr<const DEMTile>,
                       GeoTileHash> tiles_;
};

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
