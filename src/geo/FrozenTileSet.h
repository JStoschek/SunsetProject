#pragma once
#include <cstddef>
#include <memory>
#include <unordered_map>

#include "GeoTile.h"

// An immutable, eviction-free set of data tiles keyed by GeoTile, sized to
// exactly one strip's working set.  Built once (serially) before a strip's
// parallel region and read concurrently by many per-worker views.  Performs no
// eviction: every working-set tile stays resident for the whole strip, so a
// worker can never read a tile another worker freed.  Tile buffers are shared
// (shared_ptr) so freezing a tile already resident in a loader's cache costs no
// reload / re-rasterization, and adjacent strips reuse the same data.
//
// `Payload` is the per-tile data — DEMTile for elevation, vector<uint64_t> for
// the packed ocean-mask bits.  The cursor fast path and the sampling / miss
// policy differ between the two and live in each concrete view (FrozenDEMView,
// FrozenOceanView), which are deliberately kept hand-written.
template <class Payload>
class FrozenTileSet {
public:
    // Returns the payload for `key`, or nullptr if it was not frozen (e.g. a
    // coordinate outside the working set).
    const Payload* find(GeoTile key) const {
        auto it = tiles_.find(key);
        return it == tiles_.end() ? nullptr : it->second.get();
    }

    // Populate the set.  Buffers are shared so a tile already resident in a
    // loader's cache (or an adjacent strip's set) is reused with no copy.
    void insert(GeoTile key, std::shared_ptr<const Payload> payload) {
        tiles_.emplace(key, std::move(payload));
    }

    std::size_t tile_count() const { return tiles_.size(); }

private:
    std::unordered_map<GeoTile, std::shared_ptr<const Payload>, GeoTileHash>
        tiles_;
};
