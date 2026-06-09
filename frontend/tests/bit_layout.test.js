'use strict';
// Frontend half of the bit-layout wire contract (ADR-0013). Driven by the shared
// committed vectors (src/bit_layout/vectors.json) — the SAME file the C++ test
// asserts against (via a generated header), so the JS reader and the C++ packer
// cannot silently disagree on bit order or layout.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const BitLayout = require('../bit_layout.js');
const vectors = JSON.parse(
  fs.readFileSync(path.join(__dirname, '../../src/bit_layout/vectors.json'), 'utf8'),
);

const configByName = Object.fromEntries(vectors.configs.map((c) => [c.name, c]));
function layoutFor(name) {
  const c = configByName[name];
  return BitLayout.fromConfig(c.azimuth_min_deg, c.azimuth_max_deg, c.azimuth_step_deg);
}

// ── Cycle 1: bit_count / bytes_per_pixel / flag index from (min,max,step) ─────
test('bit_count / bytes_per_pixel / flag index match the shared vectors', () => {
  for (const c of vectors.configs) {
    const layout = BitLayout.fromConfig(
      c.azimuth_min_deg, c.azimuth_max_deg, c.azimuth_step_deg);
    assert.strictEqual(layout.bitCount, c.bit_count, c.name + ' bit_count');
    assert.strictEqual(layout.bytesPerPixel, c.bytes_per_pixel, c.name + ' bytes_per_pixel');
    assert.strictEqual(layout.flagBitIndex, c.flag_bit_index, c.name + ' flag_bit_index');
  }
});

// ── Cycle 2: azimuth -> (bit index, byte offset, mask) ───────────────────────
test('azimuth -> (bit index, byte offset, mask) match the shared vectors', () => {
  for (const v of vectors.azimuth_to_bit) {
    const layout = layoutFor(v.config);
    const idx = layout.azimuthToBitIndex(v.azimuth);
    assert.strictEqual(idx, v.bit_index, v.config + ' az ' + v.azimuth + ' index');
    assert.strictEqual(BitLayout.byteOffset(idx), v.byte_offset, 'byte offset');
    assert.strictEqual(BitLayout.bitMask(idx), v.mask, 'mask');
  }
});

// ── Cycle 3: round-trip — set azimuth A's bit; A reads visible, neighbours not ─
test('set azimuth A\'s bit -> A visible, immediate neighbours not', () => {
  for (const v of vectors.azimuth_to_bit) {
    const layout = layoutFor(v.config);
    const pixel = new Uint8Array(layout.bytesPerPixel);

    const idx = layout.azimuthToBitIndex(v.azimuth);
    BitLayout.setBit(pixel, idx);

    assert.ok(BitLayout.getBit(pixel, idx), 'A visible');
    if (idx - 1 >= 0) assert.ok(!BitLayout.getBit(pixel, idx - 1), 'lower neighbour clear');
    if (idx + 1 < layout.bitCount) assert.ok(!BitLayout.getBit(pixel, idx + 1), 'upper neighbour clear');
  }
});

// ── Cycle 4: azimuths outside [min, max] are rejected, not clamped ────────────
test('azimuths outside the window are rejected, never clamped', () => {
  for (const v of vectors.rejected) {
    const layout = layoutFor(v.config);
    assert.strictEqual(layout.azimuthToBitIndex(v.azimuth), null,
      v.config + ' az ' + v.azimuth + ' should be rejected');
  }
});

// ── Cycle 5: land/data flag at bit_count, never collides with a visibility bit ─
test('land/data flag at bit_count, never collides with a visibility bit', () => {
  for (const f of vectors.flag_bit) {
    const layout = layoutFor(f.config);
    assert.strictEqual(layout.flagBitIndex, f.bit_index, 'flag index');
    assert.strictEqual(BitLayout.byteOffset(layout.flagBitIndex), f.byte_offset, 'flag byte');
    assert.strictEqual(BitLayout.bitMask(layout.flagBitIndex), f.mask, 'flag mask');

    // Setting every visibility bit leaves the flag clear...
    const allVis = new Uint8Array(layout.bytesPerPixel);
    for (let i = 0; i < layout.bitCount; i++) BitLayout.setBit(allVis, i);
    assert.ok(!layout.getFlag(allVis), 'flag clear when only visibility bits set');

    // ...and setting only the flag leaves every visibility bit clear.
    const onlyFlag = new Uint8Array(layout.bytesPerPixel);
    layout.setFlag(onlyFlag);
    assert.ok(layout.getFlag(onlyFlag), 'flag reads back set');
    for (let i = 0; i < layout.bitCount; i++) {
      assert.ok(!BitLayout.getBit(onlyFlag, i), 'visibility bit ' + i + ' clear');
    }
  }
});
