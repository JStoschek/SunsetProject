// Unit test for AzimuthRangeAccumulator. Uses hardcoded AzimuthSlice objects
// — no DEM tiles or GSHHG data required.
//
// The accumulator packs an exact per-pixel azimuth bitmask (ADR-0013): for the
// i-th swept azimuth it sets visibility bit i (via the BitLayout wire contract)
// for every visible pixel, into an n × bytes_per_pixel byte buffer. This is the
// representation that — unlike the old [min_az, max_az] arc — can record a
// pixel that is visible, then blocked, then visible again across the sweep.
//
// Three-pixel grid (width=3, height=1) swept over a small azimuth window.

#include "AzimuthRangeAccumulator.h"

#include "BitLayout.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

AzimuthSlice make_slice(std::vector<bool> visible) {
    AzimuthSlice s;
    s.width   = static_cast<int>(visible.size());
    s.height  = 1;
    s.visible = std::move(visible);
    return s;
}

// Read visibility bit `idx` of pixel `p` out of the accumulator's packed buffer.
bool pixel_bit(const AzimuthRangeAccumulator& acc, int p, int idx) {
    const std::uint8_t* pixel = acc.mask.data() +
        static_cast<std::size_t>(p) * acc.layout.bytes_per_pixel;
    return BitLayout::get_bit(pixel, idx);
}

}  // namespace

int main() {
    // A 5-azimuth window 265°…269° step 1° → bits 0…4 map to those azimuths.
    const BitLayout layout = BitLayout::from_config(265.0, 269.0, 1.0);
    assert(layout.bit_count == 5);

    // ── Cycle 1: one slice sets bit i for each visible pixel ──────────────
    // The i-th azimuth (265° → bit 0) sets bit 0 on every visible pixel and
    // leaves it clear on the rest.
    {
        AzimuthRangeAccumulator acc(3, layout);

        // Pixels 0 and 2 visible at 265°; pixel 1 not.
        acc.accumulate(make_slice({true, false, true}), 265.0);

        assert(pixel_bit(acc, 0, 0)  && "P0 visible at 265 → bit 0 set");
        assert(!pixel_bit(acc, 1, 0) && "P1 not visible at 265 → bit 0 clear");
        assert(pixel_bit(acc, 2, 0)  && "P2 visible at 265 → bit 0 set");
        std::puts("PASS: one slice sets bit i for each visible pixel");
    }

    // ── Cycle 2: visible → blocked → visible across the sweep ─────────────
    // The old min/max arc would fill bit 1 back in (min 265, max 267 spans the
    // gap); the bitmask must leave the blocked azimuth's bit clear.
    {
        AzimuthRangeAccumulator acc(3, layout);

        // Pixel 0: visible 265°, blocked 266°, visible 267°, blocked 268°/269°.
        acc.accumulate(make_slice({true,  false, false}), 265.0);  // bit 0
        acc.accumulate(make_slice({false, false, false}), 266.0);  // bit 1 — gap
        acc.accumulate(make_slice({true,  false, false}), 267.0);  // bit 2
        acc.accumulate(make_slice({false, false, false}), 268.0);
        acc.accumulate(make_slice({false, false, false}), 269.0);

        assert(pixel_bit(acc, 0, 0)  && "P0 visible at 265 → bit 0 set");
        assert(!pixel_bit(acc, 0, 1) && "P0 BLOCKED at 266 → bit 1 clear (the gap)");
        assert(pixel_bit(acc, 0, 2)  && "P0 visible at 267 → bit 2 set");
        assert(!pixel_bit(acc, 0, 3) && "P0 blocked at 268 → bit 3 clear");
        assert(!pixel_bit(acc, 0, 4) && "P0 blocked at 269 → bit 4 clear");
        std::puts("PASS: visible→blocked→visible keeps the gap bit clear");
    }

    // ── Cycle 3: a never-visible pixel stays all-zero ─────────────────────
    {
        AzimuthRangeAccumulator acc(3, layout);

        acc.accumulate(make_slice({true, true, false}), 265.0);
        acc.accumulate(make_slice({true, true, false}), 266.0);
        acc.accumulate(make_slice({true, true, false}), 267.0);

        for (int b = 0; b < layout.bit_count; ++b)
            assert(!pixel_bit(acc, 2, b) && "P2 never visible → every bit clear");
        std::puts("PASS: never-visible pixel keeps all visibility bits clear");
    }

    std::puts("ALL PASS");
    return 0;
}
