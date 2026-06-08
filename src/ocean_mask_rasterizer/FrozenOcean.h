#pragma once
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

#include "FrozenTileSet.h"
#include "GeoTile.h"
#include "OceanMaskRasterizer.h"  // OceanOriginResult
#include "OceanSampling.h"

// An immutable set of rasterized ocean-mask tiles for one strip's working set.
// Just a named FrozenTileSet of packed water-bit buffers (built once by
// OceanMaskRasterizer::freeze, read concurrently by many FrozenOceanView
// handles, never evicted) — the is_water cursor, the not-frozen-is-water policy,
// and the coast march live in FrozenOceanView below.
class FrozenOcean : public FrozenTileSet<std::vector<uint64_t>> {};

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
