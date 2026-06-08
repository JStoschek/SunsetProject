#pragma once
#include <climits>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DEMTile.h"
#include "FrozenDEM.h"

class DEMTileLoader {
public:
    explicit DEMTileLoader(const std::string& tile_dir, int lru_capacity = 8);
    float get_elevation(double lat, double lon);
    std::size_t tile_count() const { return index_.size(); }

    // Build a frozen, eviction-free snapshot of the tiles in `keys` (the strip
    // working set).  Loads each tile fresh; the stateful LRU cache is left
    // untouched for the serial phases.  Keys with no matching tile file are
    // skipped (their reads return no-data via FrozenDEM::find).
    FrozenDEM freeze(const std::set<GeoTile>& keys);

private:
    using TileKey  = GeoTile;
    using PairHash = GeoTileHash;
    using TileData = DEMTile;

    struct CacheEntry {
        std::shared_ptr<const TileData> data;
        std::list<TileKey>::iterator    lru_it;
    };

    std::unordered_map<TileKey, std::string, PairHash>    index_;
    std::unordered_map<TileKey, CacheEntry, PairHash>     cache_;
    std::list<TileKey>                                     lru_list_;
    int lru_capacity_;

    // Last-tile fast path: skips both index_.find and cache_.find when
    // consecutive get_elevation calls land in the same DEM tile (the common
    // case during a horizon-sweep march).
    TileKey         last_key_{INT_MAX, INT_MAX};
    const TileData* last_tile_ = nullptr;  // null after eviction or first call

    // Load `key` into the cache (or return the resident copy), updating the LRU.
    // The shared buffer lets freeze() hand the same data to a FrozenDEM with no
    // reload or copy.
    std::shared_ptr<const TileData> get_or_load(const TileKey& key,
                                                const std::string& path);
};
