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

/// A compact reference to a GSHHG polygon: its byte offset in the file and
/// the number of points (so the caller can seek past its point data without
/// re-reading the header).
struct PolyRef {
    std::streamoff offset;  ///< byte offset of the GshhgHeader in the file
    int            n;       ///< point count
};

/// Result of OceanMaskRasterizer::ocean_origin_for_ray.
/// Contains both the 200 km-offset ocean origin point and the coastline
/// crossing where is_water first returned false during the march.
struct OceanOriginResult {
    double origin_lat;  ///< latitude of the ocean origin (200 km offset point)
    double origin_lon;  ///< longitude of the ocean origin
    double coast_lat;   ///< latitude of the coastline crossing (first land pixel)
    double coast_lon;   ///< longitude of the coastline crossing
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

    /// Marches along azimuth_deg from (lat, lon) until hitting land (the
    /// coastline crossing), then returns both the crossing point and a point
    /// 200 km back along the reverse azimuth from that crossing.
    /// (lat, lon) must already be in the ocean.
    OceanOriginResult ocean_origin_for_ray(double azimuth_deg,
                                            double lat, double lon,
                                            double step_km  = 1.0,
                                            double max_km   = 100.0);

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

    /// Rasterize all Level-1 polygons that overlap the 1°×1° tile at
    /// (tile_lat, tile_lon) into `raster` using the pre-built spatial index.
    void rasterize_tile(int tile_lat, int tile_lon,
                        std::vector<uint8_t>& raster);

    std::unordered_map<TileKey, CacheEntry,            PairHash> cache_;
    std::unordered_map<TileKey, std::vector<PolyRef>,  PairHash> index_;
    std::list<TileKey>                                            lru_list_;

    // -------------------------------------------------------------------------
    std::ifstream file_;
    int           lru_capacity_;
    std::string   path_;
    int           rasterize_count_ = 0;
};
