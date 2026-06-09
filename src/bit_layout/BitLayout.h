#pragma once
// BitLayout — the single source of the per-pixel azimuth-bitmask packing math
// (ADR-0013). The engine packer and the frontend reader both derive bit_count,
// bytes_per_pixel, and azimuth<->bit-index<->(byte,mask) from here; nothing else
// may re-derive the packing ad hoc. Mirrored in frontend/bit_layout.js and pinned
// by the shared vectors.json.
//
// Each pixel is bit_count + 1 bits: bit i (i in [0, bit_count)) = visible at
// azimuth azimuth_min_deg + i*azimuth_step_deg; the bit at index bit_count is the
// land/data flag. Bits are packed LSB-first within each byte.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

struct BitLayout {
    double azimuth_min_deg;
    double azimuth_max_deg;
    double azimuth_step_deg;
    int bit_count;        // round((max - min) / step) + 1
    int flag_bit_index;   // == bit_count
    int bytes_per_pixel;  // ceil((bit_count + 1) / 8)

    static BitLayout from_config(double min_deg, double max_deg, double step_deg) {
        BitLayout l;
        l.azimuth_min_deg = min_deg;
        l.azimuth_max_deg = max_deg;
        l.azimuth_step_deg = step_deg;
        l.bit_count = round_half_up((max_deg - min_deg) / step_deg) + 1;
        l.flag_bit_index = l.bit_count;
        // +1 for the land/data flag that follows the bit_count visibility bits.
        l.bytes_per_pixel = (l.bit_count + 1 + 7) / 8;
        return l;
    }

    // Azimuth -> visibility bit index, or nullopt if the azimuth falls outside
    // the window (index not in [0, bit_count)). Never clamps an out-of-range
    // azimuth onto the nearest edge bit.
    std::optional<int> azimuth_to_bit_index(double az) const {
        const int idx = round_half_up((az - azimuth_min_deg) / azimuth_step_deg);
        if (idx < 0 || idx >= bit_count) return std::nullopt;
        return idx;
    }

    // Bit index -> location within a pixel's byte span, LSB-first. Valid for any
    // index in [0, bytes_per_pixel*8), including the land/data flag index.
    static int byte_offset(int bit_index) { return bit_index >> 3; }
    static std::uint8_t bit_mask(int bit_index) {
        return static_cast<std::uint8_t>(1u << (bit_index & 7));
    }

    // Set / test the bit at an index within a pixel's byte span (LSB-first).
    static void set_bit(std::uint8_t* pixel, int bit_index) {
        pixel[byte_offset(bit_index)] |= bit_mask(bit_index);
    }
    static bool get_bit(const std::uint8_t* pixel, int bit_index) {
        return (pixel[byte_offset(bit_index)] & bit_mask(bit_index)) != 0;
    }

    // Set / test the land/data flag (bit index bit_count): clear = transparent
    // water/no-data, set = opaque land.
    void set_flag(std::uint8_t* pixel) const { set_bit(pixel, flag_bit_index); }
    bool get_flag(const std::uint8_t* pixel) const { return get_bit(pixel, flag_bit_index); }

    // Round half up via floor(x + 0.5). This MUST match JS Math.round exactly:
    // C++ std::round rounds half away from zero, which diverges from JS on
    // negative half-values (an azimuth just below the window) — the kind of
    // silent C++/JS drift this contract exists to prevent.
    static int round_half_up(double x) {
        return static_cast<int>(std::floor(x + 0.5));
    }
};
