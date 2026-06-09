#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "AzimuthRangeSweep.h"
#include "BitLayout.h"

/// Resolve the three display states (ADR-0013) by setting the land/data flag on
/// each pixel of a swept strip.  After the sweep, every pixel holds only
/// visibility bits; this post-process decides transparency:
///
///   is_water(lat, lon) == true → pixel zeroed (flag clear → transparent);
///                                water transparency dominates any computed
///                                visibility bits.
///   not water                  → flag bit set (index bit_count); the computed
///                                visibility bits are left intact, so land that
///                                never saw the sunset stays flag-set with all
///                                visibility bits clear (opaque black).
///
/// `water_at` is called once per pixel; production passes OceanMaskRasterizer,
/// tests pass a trivial lambda — no DEM tiles or GSHHG required for tests.
template<typename IsWater>
void apply_land_data_flag(StripResult& result, const BitLayout& layout,
                          double strip_min_lat, double min_lon, double cell_deg,
                          IsWater water_at) {
    const int w   = result.width;
    const int h   = result.height;
    const int bpp = layout.bytes_per_pixel;

    for (int r = 0; r < h; ++r) {
        const double lat = strip_min_lat + r * cell_deg;
        for (int c = 0; c < w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * w + c;
            const double lon = min_lon + c * cell_deg;
            std::uint8_t* pixel = result.mask.data() + i * bpp;

            if (water_at(lat, lon)) {
                std::fill(pixel, pixel + bpp, std::uint8_t{0});
            } else {
                layout.set_flag(pixel);
            }
        }
    }
}
