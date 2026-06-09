// Unit tests for BitLayout — the azimuth-bitmask wire contract (ADR-0013).
//
// Every assertion is driven by the shared committed vectors (vectors.json,
// transcoded to bit_layout_vectors.h), so this C++ test and the JS test in
// frontend/tests/bit_layout.test.js pin the SAME numbers. If the two packings
// ever drift, one of the two suites goes red.

#include "BitLayout.h"
#include "bit_layout_vectors.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace bit_layout_vectors;

// Look up a config vector by name and build the BitLayout under test from it.
static BitLayout layout_for(const char* name) {
    for (const auto& c : kConfigs) {
        if (std::strcmp(c.name, name) == 0) {
            return BitLayout::from_config(
                c.azimuth_min_deg, c.azimuth_max_deg, c.azimuth_step_deg);
        }
    }
    assert(false && "unknown config name in vectors");
    return BitLayout::from_config(0, 0, 1);
}

// ── Cycle 1: bit_count / bytes_per_pixel / flag index from (min,max,step) ─────
static void test_config_math() {
    for (const auto& c : kConfigs) {
        const BitLayout layout =
            BitLayout::from_config(c.azimuth_min_deg, c.azimuth_max_deg, c.azimuth_step_deg);
        assert(layout.bit_count == c.bit_count);
        assert(layout.bytes_per_pixel == c.bytes_per_pixel);
        assert(layout.flag_bit_index == c.flag_bit_index);
    }
    std::puts("PASS: bit_count / bytes_per_pixel / flag index match the shared vectors");
}

// ── Cycle 2: azimuth -> (bit index, byte offset, mask) ───────────────────────
static void test_azimuth_to_bit() {
    for (const auto& v : kAzimuthToBit) {
        const BitLayout layout = layout_for(v.config);
        const std::optional<int> idx = layout.azimuth_to_bit_index(v.azimuth);
        assert(idx.has_value());
        assert(*idx == v.bit_index);
        assert(BitLayout::byte_offset(*idx) == v.byte_offset);
        assert(BitLayout::bit_mask(*idx) == v.mask);
    }
    std::puts("PASS: azimuth -> (bit index, byte offset, mask) match the shared vectors");
}

// ── Cycle 3: round-trip — set azimuth A's bit; A reads visible, neighbours not ─
static void test_round_trip_isolates_neighbours() {
    for (const auto& v : kAzimuthToBit) {
        const BitLayout layout = layout_for(v.config);
        std::vector<std::uint8_t> pixel(layout.bytes_per_pixel, 0);

        const int idx = *layout.azimuth_to_bit_index(v.azimuth);
        BitLayout::set_bit(pixel.data(), idx);

        assert(BitLayout::get_bit(pixel.data(), idx));          // A is visible
        if (idx - 1 >= 0)                                       // lower neighbour clear
            assert(!BitLayout::get_bit(pixel.data(), idx - 1));
        if (idx + 1 < layout.bit_count)                         // upper neighbour clear
            assert(!BitLayout::get_bit(pixel.data(), idx + 1));
    }
    std::puts("PASS: set azimuth A's bit -> A visible, immediate neighbours not");
}

// ── Cycle 4: azimuths outside [min, max] are rejected, not clamped ────────────
static void test_out_of_window_rejected() {
    for (const auto& v : kRejected) {
        const BitLayout layout = layout_for(v.config);
        assert(!layout.azimuth_to_bit_index(v.azimuth).has_value());
    }
    std::puts("PASS: azimuths outside the window are rejected, never clamped");
}

// ── Cycle 5: land/data flag at bit_count, never collides with a visibility bit ─
static void test_flag_bit_no_collision() {
    for (const auto& f : kFlagBits) {
        const BitLayout layout = layout_for(f.config);
        assert(layout.flag_bit_index == f.bit_index);
        assert(BitLayout::byte_offset(layout.flag_bit_index) == f.byte_offset);
        assert(BitLayout::bit_mask(layout.flag_bit_index) == f.mask);

        // Setting every visibility bit leaves the flag clear...
        std::vector<std::uint8_t> all_vis(layout.bytes_per_pixel, 0);
        for (int i = 0; i < layout.bit_count; ++i) BitLayout::set_bit(all_vis.data(), i);
        assert(!layout.get_flag(all_vis.data()));

        // ...and setting only the flag leaves every visibility bit clear.
        std::vector<std::uint8_t> only_flag(layout.bytes_per_pixel, 0);
        layout.set_flag(only_flag.data());
        assert(layout.get_flag(only_flag.data()));
        for (int i = 0; i < layout.bit_count; ++i)
            assert(!BitLayout::get_bit(only_flag.data(), i));
    }
    std::puts("PASS: land/data flag at bit_count, never collides with a visibility bit");
}

int main() {
    test_config_math();
    test_azimuth_to_bit();
    test_round_trip_isolates_neighbours();
    test_out_of_window_rejected();
    test_flag_bit_no_collision();
    std::puts("All BitLayout tests passed.");
    return 0;
}
