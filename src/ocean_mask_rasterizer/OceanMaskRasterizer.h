#pragma once
#include <cstdint>
#include <fstream>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// GSHHG binary header (big-endian on disk).
// See data/GSHHG/data/0-data/README.TXT for the full format spec.
struct GshhgHeader {
    int32_t id;         // polygon id, starting at 0
    int32_t n;          // number of points
    int32_t flag;       // level | version<<8 | greenwich<<16 | source<<24 | river<<25
    int32_t west;       // extent in micro-degrees
    int32_t east;
    int32_t south;
    int32_t north;
    int32_t area;       // 1/10 km²
    int32_t area_full;
    int32_t container;
    int32_t ancestor;
};

struct GshhgPoint {
    int32_t x;  // longitude in micro-degrees
    int32_t y;  // latitude in micro-degrees
};

class OceanMaskRasterizer {
public:
    /// Opens a GSHHG binary file (e.g. gshhs_f.b) and holds it open for the
    /// object's lifetime.  The file is validated by reading the first polygon
    /// header; throws std::runtime_error with a descriptive message if the
    /// file cannot be opened or does not look like a GSHHG binary.
    explicit OceanMaskRasterizer(const std::string& gshhg_full_path,
                                  int lru_capacity = 8);

    // Non-copyable (owns an open file stream).
    OceanMaskRasterizer(const OceanMaskRasterizer&)            = delete;
    OceanMaskRasterizer& operator=(const OceanMaskRasterizer&) = delete;

    /// Returns true if (lat, lon) falls on any water body (ocean, bay,
    /// estuary).  Looks up the 1°×1° tile in an LRU cache; rasterizes from
    /// GSHHG on a cache miss.
    bool is_water(double lat, double lon);

    /// Returns the ocean-origin point for a ray cast at azimuth_deg from
    /// (lat, lon): 200 km west of the first land→water crossing along the ray.
    /// Stub: returns {lat, lon} unchanged.
    std::pair<double, double> ocean_origin_for_ray(double azimuth_deg,
                                                    double lat, double lon);

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
    // Size: (kTilePixels² + 63) / 64  ≈ 1.82 M words ≈ 14 MB per tile.
    struct CacheEntry {
        std::vector<uint64_t>        bits;
        std::list<TileKey>::iterator lru_it;
    };

    std::unordered_map<TileKey, CacheEntry, PairHash> cache_;
    std::list<TileKey>                                 lru_list_;

    // -------------------------------------------------------------------------
    std::ifstream file_;
    int           lru_capacity_;
    std::string   path_;
    int           rasterize_count_ = 0;
};
