#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "OceanMaskRasterizer.h"  // OceanOriginResult

// Shared, pure read helpers for the ocean mask.  Used by both the stateful
// OceanMaskRasterizer (is_water / ocean_origin_for_ray) and the frozen
// per-strip path (FrozenOceanView), so frozen reads are identical to the
// stateful loader's by construction.

// 1° / (1/3″) = 10800 pixels per tile edge.
inline constexpr int kOceanTilePixels = 10800;

// Read the water bit for (lat, lon) from a packed 1-bit-per-pixel tile whose
// north-west corner is (tile_lat+1, tile_lon).  Bit i (= row*W + col) is 1 for
// water; pixel coordinates are clamped to the tile extent.
inline bool sample_water(const uint64_t* bits,
                         int tile_lat, int tile_lon,
                         double lat, double lon) {
    int col = (int)((lon - tile_lon) * kOceanTilePixels);
    int row = (int)((tile_lat + 1.0 - lat) * kOceanTilePixels);
    col = std::max(0, std::min(col, kOceanTilePixels - 1));
    row = std::max(0, std::min(row, kOceanTilePixels - 1));
    const int idx = row * kOceanTilePixels + col;
    return (bits[idx / 64] >> (idx % 64)) & 1ULL;
}

// Spherical-Earth forward formula: destination given start (lat, lon degrees),
// bearing (degrees clockwise from north), and distance (km).  R = 6371 km.
inline std::pair<double, double> geo_destination(double lat, double lon,
                                                 double bearing_deg,
                                                 double dist_km) {
    const double R     = 6371.0;
    const double d     = dist_km / R;
    const double lat1  = lat         * M_PI / 180.0;
    const double lon1  = lon         * M_PI / 180.0;
    const double theta = bearing_deg * M_PI / 180.0;

    const double lat2 = std::asin(std::sin(lat1) * std::cos(d)
                                  + std::cos(lat1) * std::sin(d) * std::cos(theta));
    const double lon2 = lon1 + std::atan2(std::sin(theta) * std::sin(d) * std::cos(lat1),
                                          std::cos(d) - std::sin(lat1) * std::sin(lat2));
    return { lat2 * 180.0 / M_PI, lon2 * 180.0 / M_PI };
}

// March along `azimuth_deg` from (lat, lon) until `is_water` first returns false
// (the coastline crossing) and return that crossing.  If no coast is found
// within max_km, returns the input point.  `is_water` is any callable
// bool(double lat, double lon) — the stateful rasterizer or a frozen view.
//
// The seed (lat, lon) is open water (dist 0).  Returning the first land *sample*
// directly biases the crossing up to a full `step_km` inland of the true
// waterline; the engine then treats that step-wide band of real coastal land as
// seaward of the crossing (dist < 0) and marks it never-visible — a thin dead
// strip along the whole coast.  Once the first land sample brackets the
// shoreline, bisect the water/land interval down to ~one mask cell and return
// the land side, so the crossing sits at the waterline (still on land) instead
// of up to a step inland.
template <class IsWater>
OceanOriginResult march_to_coast(double azimuth_deg, double lat, double lon,
                                 double step_km, double max_km,
                                 IsWater&& is_water) {
    constexpr double kCrossingTolKm = 0.005;  // ≈ 5 m, below the ~10 m mask cell
    double water = 0.0;  // last distance known to be water (starts at the seed)
    for (double dist = step_km; dist <= max_km; dist += step_km) {
        auto [pt_lat, pt_lon] = geo_destination(lat, lon, azimuth_deg, dist);
        if (!is_water(pt_lat, pt_lon)) {
            // Bisect [water, dist] (water → land) toward the shoreline; `hi`
            // always stays on the land side, so the returned point is on land.
            double lo = water, hi = dist;
            while (hi - lo > kCrossingTolKm) {
                const double mid = 0.5 * (lo + hi);
                auto [m_lat, m_lon] = geo_destination(lat, lon, azimuth_deg, mid);
                if (is_water(m_lat, m_lon)) lo = mid; else hi = mid;
            }
            auto [c_lat, c_lon] = geo_destination(lat, lon, azimuth_deg, hi);
            return { c_lat, c_lon };
        }
        water = dist;
    }
    return { lat, lon };
}
