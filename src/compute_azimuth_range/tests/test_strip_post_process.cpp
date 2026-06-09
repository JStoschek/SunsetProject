// Behavioral tests for apply_land_data_flag (StripPostProcess.h).
//
// After the bitmask sweep, every pixel holds only visibility bits (no flag).
// The post-process resolves the three display states (ADR-0013) by setting the
// land/data flag bit (index bit_count):
//
//   water / no-data       → pixel zeroed: flag clear → transparent
//   never-visible land     → flag set, all visibility bits clear → opaque black
//   visible land           → flag set, visibility bits preserved → date-dependent
//
// Water transparency dominates any computed visibility: a water pixel that the
// sweep marked visible is still forced transparent.

#include "StripPostProcess.h"

#include "BitLayout.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// A 5-azimuth window so a pixel has bits 0..4 plus the flag at index 5.
const BitLayout kLayout = BitLayout::from_config(265.0, 269.0, 1.0);

// Build a 1×1 StripResult whose single pixel has the given visibility bits set.
StripResult make_result(std::vector<int> visible_bits) {
    StripResult r;
    r.width           = 1;
    r.height          = 1;
    r.bytes_per_pixel = kLayout.bytes_per_pixel;
    r.mask.assign(kLayout.bytes_per_pixel, 0);
    for (int b : visible_bits) BitLayout::set_bit(r.mask.data(), b);
    return r;
}

bool any_visibility_bit(const StripResult& r) {
    for (int b = 0; b < kLayout.bit_count; ++b)
        if (BitLayout::get_bit(r.mask.data(), b)) return true;
    return false;
}

}  // namespace

int main() {
    const double lat = 37.0, lon = -122.5, cell = 1.0 / 3600.0;
    auto always_water = [](double, double) { return true; };
    auto never_water  = [](double, double) { return false; };

    // ── 1. Water pixel → flag clear, transparent ──────────────────────────
    {
        StripResult r = make_result({});  // no visibility, but is water
        apply_land_data_flag(r, kLayout, lat, lon, cell, always_water);

        assert(!kLayout.get_flag(r.mask.data()) && "water: flag clear");
        std::puts("PASS: water pixel → flag clear (transparent)");
    }

    // ── 2. Never-visible land → flag set, all visibility bits clear ───────
    {
        StripResult r = make_result({});  // land that earned no visible azimuth
        apply_land_data_flag(r, kLayout, lat, lon, cell, never_water);

        assert(kLayout.get_flag(r.mask.data()) && "never-visible land: flag set");
        assert(!any_visibility_bit(r) && "never-visible land: no visibility bits");
        std::puts("PASS: never-visible land → flag set, visibility bits clear");
    }

    // ── 3. Visible land → flag set, the computed bits preserved ───────────
    {
        StripResult r = make_result({0, 2});  // visible at 265° and 267°
        apply_land_data_flag(r, kLayout, lat, lon, cell, never_water);

        assert(kLayout.get_flag(r.mask.data()) && "visible land: flag set");
        assert(BitLayout::get_bit(r.mask.data(), 0)  && "bit 0 preserved");
        assert(!BitLayout::get_bit(r.mask.data(), 1) && "gap bit 1 still clear");
        assert(BitLayout::get_bit(r.mask.data(), 2)  && "bit 2 preserved");
        std::puts("PASS: visible land → flag set, visibility bits preserved");
    }

    // ── 4. Water dominates computed visibility ────────────────────────────
    // The sweep marked this pixel visible, but the Ocean Mask says water — it
    // must end fully transparent (flag clear AND no stray visibility bits).
    {
        StripResult r = make_result({0, 2, 4});  // computed visible, yet water
        apply_land_data_flag(r, kLayout, lat, lon, cell, always_water);

        assert(!kLayout.get_flag(r.mask.data()) && "water dominates: flag clear");
        assert(!any_visibility_bit(r) && "water dominates: visibility bits cleared");
        std::puts("PASS: water dominates computed visibility (fully transparent)");
    }

    std::puts("ALL PASS");
    return 0;
}
