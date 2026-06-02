#pragma once
#include <cmath>
#include <limits>

#include "AzimuthRangeSweep.h"

/// Apply the water-transparency and never-visible-land sentinels to a strip.
///
/// After the azimuth sweep, each pixel's (min_az, max_az) is in one of three
/// raw states: (finite, finite), (NaN, NaN), or an already-encoded sentinel.
/// This function resolves the final three-state encoding:
///
///   is_water(lat, lon) == true  → (NaN,   NaN)   transparent (ocean / inland)
///   both NaN and not water      → (+inf, -inf)   never-visible land sentinel
///   finite and not water        → unchanged      valid azimuth range preserved
///
/// `water_at` is called once per pixel; production passes OceanMaskRasterizer,
/// tests pass a trivial lambda — no DEM tiles or GSHHG required for tests.
template<typename IsWater>
void apply_water_mask(StripResult& result,
                      double strip_min_lat, double min_lon, double cell_deg,
                      IsWater water_at) {
    const float pos_inf = std::numeric_limits<float>::infinity();
    const float neg_inf = -std::numeric_limits<float>::infinity();
    const float nan_f   = std::numeric_limits<float>::quiet_NaN();

    const int w = result.width;
    const int h = result.height;

    for (int r = 0; r < h; ++r) {
        const double lat = strip_min_lat + r * cell_deg;
        for (int c = 0; c < w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * w + c;
            const double lon = min_lon + c * cell_deg;

            if (water_at(lat, lon)) {
                result.min_az_buf[i] = nan_f;
                result.max_az_buf[i] = nan_f;
            } else if (std::isnan(result.min_az_buf[i])) {
                result.min_az_buf[i] = pos_inf;
                result.max_az_buf[i] = neg_inf;
            }
            // else: finite land range — leave unchanged
        }
    }
}
