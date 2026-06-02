#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "WaterPolygonSource.h"

class FrozenOcean;  // defined in FrozenOcean.h; returned by freeze()

/// Result of OceanMaskRasterizer::ocean_origin_for_ray.
/// Contains the coastline crossing where is_water first returned false.
struct OceanOriginResult {
    double coast_lat;   ///< latitude of the coastline crossing (first land pixel)
    double coast_lon;   ///< longitude of the coastline crossing
};

class OceanMaskRasterizer {
public:
    /// Convenience constructor: opens a GSHHG binary file (e.g. gshhs_f.b) and
    /// builds a GshhgWaterPolygonSource internally.  Throws std::runtime_error
    /// if the file cannot be opened or does not look like a GSHHG binary.
    explicit OceanMaskRasterizer(const std::string& gshhg_full_path,
                                  int lru_capacity = 8);

    /// Primary constructor: takes ownership of any WaterPolygonSource.
    explicit OceanMaskRasterizer(std::unique_ptr<WaterPolygonSource> source,
                                  int lru_capacity = 8);

    // Non-copyable (owns a WaterPolygonSource).
    OceanMaskRasterizer(const OceanMaskRasterizer&)            = delete;
    OceanMaskRasterizer& operator=(const OceanMaskRasterizer&) = delete;

    /// Returns true if (lat, lon) falls on any water body (ocean, bay,
    /// estuary).  Looks up the 1°×1° tile in an LRU cache; rasterizes from
    /// the source on a cache miss.
    bool is_water(double lat, double lon);

    /// Marches along azimuth_deg from (lat, lon) until hitting land and returns
    /// the coastline crossing (the first pixel where is_water is false).
    /// (lat, lon) must already be in the ocean.
    OceanOriginResult ocean_origin_for_ray(double azimuth_deg,
                                            double lat, double lon,
                                            double step_km  = 1.0,
                                            double max_km   = 100.0);

    /// Build a frozen, eviction-free snapshot of the tiles in `geo_keys` (the
    /// strip working set, in geographic-floor (floor_lat, floor_lon) keys as
    /// returned by strip_working_set).  Rasterizes each tile once; the stateful
    /// LRU cache is left untouched for the serial phases.
    FrozenOcean freeze(const std::set<std::pair<int, int>>& geo_keys);

    /// Number of times a tile was rasterized (i.e. a cache miss occurred).
    /// Useful in tests to verify that cache hits do not trigger re-rasterization.
    int rasterize_count() const { return rasterize_count_; }

private:
    // -------------------------------------------------------------------------
    // LRU tile cache
    // -------------------------------------------------------------------------
    struct PairHash {
        std::size_t operator()(std::pair<int,int> p) const noexcept {
            return std::hash<long long>{}(
                (long long)p.first << 32 | (unsigned int)p.second);
        }
    };

    using TileKey = std::pair<int,int>;   // (floor_lat, floor_lon)

    // Packed 1-bit-per-pixel tile: word[i/64] bit (i%64) is 1 for water.
    struct CacheEntry {
        std::shared_ptr<const std::vector<uint64_t>> bits;
        std::list<TileKey>::iterator                 lru_it;
    };

    /// Rasterize all land polygons that overlap the 1°×1° tile at
    /// (tile_lat, tile_lon) into `raster` using the WaterPolygonSource.
    void rasterize_tile(int tile_lat, int tile_lon,
                        std::vector<uint8_t>& raster);

    /// Cache lookup / rasterize-on-miss for one tile, updating the LRU and the
    /// last-tile cursor.
    std::shared_ptr<const std::vector<uint64_t>>
    get_or_rasterize(int tile_lat, int tile_lon);

    std::unordered_map<TileKey, CacheEntry, PairHash> cache_;
    std::list<TileKey>                                 lru_list_;

    // Last-tile fast path: avoids a hash lookup when consecutive is_water()
    // calls fall in the same 1°×1° tile (common during coast marching).
    TileKey         last_key_{INT_MAX, INT_MAX};
    const uint64_t* last_bits_ = nullptr;

    // Reused scratch buffer for rasterize_tile(); avoids a large alloc per miss.
    std::vector<uint8_t> scratch_raster_;

    // -------------------------------------------------------------------------
    std::unique_ptr<WaterPolygonSource> source_;
    int lru_capacity_;
    int rasterize_count_ = 0;
};
