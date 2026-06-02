#pragma once
#include <cmath>
#include <set>
#include <utility>

// (floor_lat, floor_lon) — matches DEMTileLoader and OceanMaskRasterizer key conventions.
using TileKey = std::pair<int, int>;

// Returns the set of 1°×1° tile keys whose geographic footprint intersects the
// latitude strip described by [min_lat, max_lat] × [min_lon, max_lon], expanded
// by tilt_margin degrees on the north and south sides.
//
// The margin accounts for the ~0.65° ray tilt introduced by non-cardinal
// azimuths (ADR-0007): a ray that starts inside the strip can exit into the
// adjacent latitude tile.  Only the latitude dimension is expanded; the
// longitude range is taken as-is from the processing bbox.
//
// Pure function — performs no file I/O and no cache access.
inline std::set<TileKey> strip_working_set(
    double min_lat, double max_lat,
    double min_lon, double max_lon,
    double tilt_margin)
{
    int lat0 = static_cast<int>(std::floor(min_lat - tilt_margin));
    int lat1 = static_cast<int>(std::floor(max_lat + tilt_margin));
    int lon0 = static_cast<int>(std::floor(min_lon));
    int lon1 = static_cast<int>(std::floor(max_lon));

    std::set<TileKey> result;
    for (int lat = lat0; lat <= lat1; ++lat)
        for (int lon = lon0; lon <= lon1; ++lon)
            result.emplace(lat, lon);
    return result;
}
