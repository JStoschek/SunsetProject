// BitLayout — the single source of the per-pixel azimuth-bitmask packing math
// (ADR-0013), frontend mirror of src/bit_layout/BitLayout.h. The packer (C++)
// and this reader both derive bit_count, bytes_per_pixel, and
// azimuth<->bit-index<->(byte,mask) from the same contract, pinned by the shared
// src/bit_layout/vectors.json.
//
// Each pixel is bit_count + 1 bits: bit i (i in [0, bit_count)) = visible at
// azimuth azimuth_min_deg + i*azimuth_step_deg; the bit at index bit_count is the
// land/data flag. Bits are packed LSB-first within each byte.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.BitLayout = factory();
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  function fromConfig(azimuthMinDeg, azimuthMaxDeg, azimuthStepDeg) {
    // Math.round is floor(x + 0.5); BitLayout.h matches it deliberately so the
    // two languages never diverge on a negative half-value.
    const bitCount = Math.round((azimuthMaxDeg - azimuthMinDeg) / azimuthStepDeg) + 1;
    return {
      azimuthMinDeg,
      azimuthMaxDeg,
      azimuthStepDeg,
      bitCount,
      flagBitIndex: bitCount,
      // +1 for the land/data flag following the bitCount visibility bits.
      bytesPerPixel: Math.ceil((bitCount + 1) / 8),

      // Azimuth -> visibility bit index, or null if the azimuth falls outside the
      // window (index not in [0, bitCount)). Never clamps onto the nearest edge.
      azimuthToBitIndex(az) {
        const idx = Math.round((az - this.azimuthMinDeg) / this.azimuthStepDeg);
        return (idx < 0 || idx >= this.bitCount) ? null : idx;
      },

      // Set / test the land/data flag (bit index bitCount): clear = transparent
      // water/no-data, set = opaque land.
      setFlag(pixel) { setBit(pixel, this.flagBitIndex); },
      getFlag(pixel) { return getBit(pixel, this.flagBitIndex); },
    };
  }

  // Bit index -> location within a pixel's byte span, LSB-first. Valid for any
  // index, including the land/data flag index.
  function byteOffset(bitIndex) { return bitIndex >> 3; }
  function bitMask(bitIndex) { return 1 << (bitIndex & 7); }

  // Set / test the bit at an index within a pixel's byte span (LSB-first).
  // pixel is a Uint8Array (or any indexable byte buffer).
  function setBit(pixel, bitIndex) { pixel[byteOffset(bitIndex)] |= bitMask(bitIndex); }
  function getBit(pixel, bitIndex) {
    return (pixel[byteOffset(bitIndex)] & bitMask(bitIndex)) !== 0;
  }

  return { fromConfig, byteOffset, bitMask, setBit, getBit };
}));
