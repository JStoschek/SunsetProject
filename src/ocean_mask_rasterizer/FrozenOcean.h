#pragma once
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "GeoTile.h"
#include "OceanMaskRasterizer.h"  // OceanOriginResult
#include "OceanSampling.h"

// An immutable set of rasterized ocean-mask tiles, sized to exactly one strip's
// working set.  Built once (serially) by OceanMaskRasterizer::freeze before a
// strip's parallel region and read concurrently by many FrozenOceanView
// handles.  Performs no eviction: every working-set tile stays resident for the
// whole strip.
class FrozenOcean {
public:
    // Returns the packed water bits for `key`, or nullptr if it was not frozen.
    const std::vector<uint64_t>* find(GeoTile key) const {
        auto it = tiles_.find(key);
        return it == tiles_.end() ? nullptr : it->second.get();
    }

    // Bits buffers are shared (shared_ptr) so freezing a tile already rasterized
    // in the loader's cache costs no re-rasterization, and adjacent strips reuse
    // the same data.
    void insert(GeoTile key, std::shared_ptr<const std::vector<uint64_t>> bits) {
        tiles_.emplace(key, std::move(bits));
    }

    std::size_t tile_count() const { return tiles_.size(); }

private:
    std::unordered_map<GeoTile, std::shared_ptr<const std::vector<uint64_t>>,
                       GeoTileHash> tiles_;
};

// A per-worker, read-only handle onto a shared FrozenOcean.  Each worker owns
// its own view so the last-tile fast path (the cursor below) survives
// parallelism with no shared mutation and no locks on the coast-finding hot
// path.
class FrozenOceanView {
public:
    explicit FrozenOceanView(const FrozenOcean& frozen) : frozen_(frozen) {}

    bool is_water(double lat, double lon) {
        const int tile_lat = (int)std::floor(lat);
        const int tile_lon = (int)std::floor(lon);
        const GeoTile key{tile_lat, tile_lon};

        const uint64_t* bits;
        if (last_bits_ && key == last_key_) {
            bits = last_bits_;
        } else {
            const std::vector<uint64_t>* t = frozen_.find(key);
            // A tile outside the working set is treated as open water so the
            // march keeps going rather than inventing a false coast.  The Slice 2
            // enumerator sizes the working set so this never happens for the
            // coordinates a strip actually touches.
            if (!t) return true;
            last_key_  = key;
            last_bits_ = t->data();
            bits = last_bits_;
        }
        return sample_water(bits, tile_lat, tile_lon, lat, lon);
    }

    OceanOriginResult ocean_origin_for_ray(double azimuth_deg,
                                           double lat, double lon,
                                           double step_km = 1.0,
                                           double max_km  = 100.0) {
        return march_to_coast(azimuth_deg, lat, lon, step_km, max_km,
                              [this](double la, double lo) { return is_water(la, lo); });
    }

private:
    const FrozenOcean& frozen_;
    GeoTile            last_key_{INT_MAX, INT_MAX};
    const uint64_t*    last_bits_ = nullptr;
};
