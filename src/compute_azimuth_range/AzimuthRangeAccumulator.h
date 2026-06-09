#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "BitLayout.h"
#include "HorizonSweepEngine.h"

/// Accumulates the exact per-pixel azimuth bitmask over a sweep of AzimuthSlices
/// (ADR-0013).  Call accumulate() once per swept azimuth; for the azimuth that
/// maps to visibility bit i (via the BitLayout wire contract), it sets bit i on
/// every pixel the slice reports visible, into an n × bytes_per_pixel byte
/// buffer.  Bits are packed LSB-first within each byte.
///
/// Unlike the old [min_az, max_az] arc, this records disjoint visibility: a
/// pixel visible, then blocked, then visible again as the sweep advances keeps a
/// gap bit clear between two set bits.  Pixels never visible stay all-zero; the
/// land/data flag bit (index bit_count) is left to the strip post-process.
struct AzimuthRangeAccumulator {
    std::vector<std::uint8_t> mask;
    BitLayout layout;
    std::size_t pixel_count;

    AzimuthRangeAccumulator(std::size_t n, const BitLayout& layout_)
        : mask(n * static_cast<std::size_t>(layout_.bytes_per_pixel), 0),
          layout(layout_), pixel_count(n) {}

    void accumulate(const AzimuthSlice& slice, double az) {
        const std::optional<int> idx = layout.azimuth_to_bit_index(az);
        if (!idx) return;  // azimuth outside the window — not a swept bit
        const int bpp = layout.bytes_per_pixel;
        for (std::size_t i = 0; i < slice.visible.size(); ++i) {
            if (slice.visible[i])
                BitLayout::set_bit(mask.data() + i * bpp, *idx);
        }
    }
};
