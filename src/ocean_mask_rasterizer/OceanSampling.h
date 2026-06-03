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
    // March incrementally, advancing latitude/longitude one step at a time and
    // scaling each longitude increment by the *local* cosine of latitude.  This
    // makes the path a true flat-earth rhumb line: stepping along it is invariant
    // to how far back the seed sits, so a far-offshore seed (wide box) reaches the
    // same crossing as a near seed (narrow box).  Recomputing from the seed each
    // sample (mean-latitude scaling over the whole span) would reintroduce the
    // box dependence we are removing.
    constexpr double R          = 6371.0;
    const double     km_per_deg = R * M_PI / 180.0;
    const double     theta      = azimuth_deg * M_PI / 180.0;
    const double     dlat_step  = (step_km * std::cos(theta)) / km_per_deg;
    const double     dE_step    = step_km * std::sin(theta) / km_per_deg;  // east (deg·cos)

    // Advance a fraction `frac` of one full step from (la, lo).  Parametrising by
    // step fraction (not by Δlat) keeps the due-east case (cos θ = 0, dlat_step =
    // 0) well-defined: latitude simply stays constant while longitude advances.
    auto step_from = [&](double la, double lo, double frac) {
        const double la2  = la + frac * dlat_step;
        const double mean = 0.5 * (la + la2) * M_PI / 180.0;
        const double lo2  = lo + frac * dE_step / std::cos(mean);
        return std::pair<double, double>{la2, lo2};
    };

    const double frac_tol = 0.0001 / step_km;  // ≈ 0.1 m / step, well below a cell

    double c_lat = lat, c_lon = lon;  // running cursor; stays the last water point
    for (double dist = step_km; dist <= max_km; dist += step_km) {
        auto [n_lat, n_lon] = step_from(c_lat, c_lon, 1.0);
        if (!is_water(n_lat, n_lon)) {
            // Bracket found: [c → water, n → land].  Bisect within this single
            // step (linear in step fraction) toward the shoreline, keeping the
            // land side so the returned point is on land — at the waterline
            // rather than up to a full step inland.
            double lo_f = 0.0;  // fraction of the step from c that is water
            double hi_f = 1.0;  // fraction of the step from c that is land
            while (hi_f - lo_f > frac_tol) {
                const double mid = 0.5 * (lo_f + hi_f);
                auto [m_lat, m_lon] = step_from(c_lat, c_lon, mid);
                if (is_water(m_lat, m_lon)) lo_f = mid; else hi_f = mid;
            }
            auto [r_lat, r_lon] = step_from(c_lat, c_lon, hi_f);  // land side
            return { r_lat, r_lon };
        }
        c_lat = n_lat; c_lon = n_lon;
    }
    return { lat, lon };
}
