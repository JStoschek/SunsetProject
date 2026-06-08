#pragma once
#include <cstddef>
#include <functional>
#include <utility>

// The identity of a single 1°×1° data tile — one square of the earth's surface,
// independent of how any dataset labels it.  Canonically the tile's south-west
// corner in signed integer degrees: south = floor(lat), west = floor(lon).
//
// Two datasets label the same square differently, so GeoTile exposes both
// labelings as named projections rather than letting callers pass a bare
// std::pair<int,int> whose convention lives only in a comment:
//
//   * floor()   -> {south, west}      signed SW corner; the Ocean Mask
//                                      rasterizer's GDAL geotransform and the
//                                      Strip Working Set use this labeling.
//   * usgs_nw() -> {south+1, -west}   USGS north-west-corner (n, |w|) that names
//                                      the DEM GeoTIFFs (USGS_13_n{n}w{w}).
//
// Trivial two-int value: constexpr, cheap to copy and compare.  The hot paths in
// DEMTileLoader / Frozen*View still compare raw ints and only materialize a
// GeoTile on a cache miss, so this stays zero-overhead against the former pair.
struct GeoTile {
    int south;  ///< floor(lat) — south edge latitude, signed
    int west;   ///< floor(lon) — west edge longitude, signed

    /// The tile that owns a geographic coordinate.
    static constexpr GeoTile owning(double lat, double lon) {
        return { ifloor(lat), ifloor(lon) };
    }
    /// From the signed SW-corner labeling (floor_lat, floor_lon).
    static constexpr GeoTile from_floor(int south_lat, int west_lon) {
        return { south_lat, west_lon };
    }
    /// From the USGS NW-corner labeling (n = floor(lat)+1, w = ceil(|lon|)).
    static constexpr GeoTile from_usgs(int north, int abs_west) {
        return { north - 1, -abs_west };
    }

    /// Signed SW-corner pair (south edge lat, west edge lon).
    constexpr std::pair<int, int> floor() const { return { south, west }; }
    /// USGS NW-corner pair (n, |w|) — used to name / look up the DEM GeoTIFFs.
    constexpr std::pair<int, int> usgs_nw() const { return { south + 1, -west }; }

    constexpr bool operator==(const GeoTile& o) const {
        return south == o.south && west == o.west;
    }
    constexpr bool operator!=(const GeoTile& o) const { return !(*this == o); }
    /// Total order so GeoTile can key a std::set (the Strip Working Set).
    constexpr bool operator<(const GeoTile& o) const {
        return south != o.south ? south < o.south : west < o.west;
    }

private:
    // constexpr floor-to-int (std::floor isn't constexpr until C++23).  Matches
    // std::floor for the bounded lat/lon range the pipeline queries.
    static constexpr int ifloor(double v) {
        const int i = static_cast<int>(v);
        return (v < i) ? i - 1 : i;
    }
};

struct GeoTileHash {
    std::size_t operator()(const GeoTile& t) const noexcept {
        return std::hash<long long>{}(
            (long long)t.south << 32 | (unsigned int)t.west);
    }
};
