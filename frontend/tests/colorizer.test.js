'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const BitLayout = require('../bit_layout.js');
const colorizeBitmaskPixel = require('../colorizer.js');

const layout = BitLayout.fromConfig(233, 309, 1.0);  // production config

function makePixel(flagSet, visibleBitIndex) {
  const px = new Uint8Array(layout.bytesPerPixel);
  if (flagSet) layout.setFlag(px);
  if (visibleBitIndex !== null) BitLayout.setBit(px, visibleBitIndex);
  return px;
}

// Cycle 1: water / no-data pixel → transparent
test('water pixel (flag clear) is transparent', () => {
  const px = makePixel(false, null);
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, 37),
    [0, 0, 0, 0],
  );
});

// Cycle 2: visible land → white
test('visible land pixel (flag + vis bit set) is white', () => {
  const px = makePixel(true, 37);
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, 37),
    [255, 255, 255, 255],
  );
});

// Cycle 3: blocked land → black
test('blocked land pixel (flag set, vis bit clear) is black', () => {
  const px = makePixel(true, null);
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, 37),
    [0, 0, 0, 255],
  );
});

// Cycle 4: disjoint pixel — visible at bit A, blocked at adjacent bit B
test('disjoint pixel: white at visible azimuth, black at adjacent blocked azimuth', () => {
  // bit 37 = azimuth 270°; set only that bit (plus flag)
  const px = makePixel(true, 37);
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, 37),
    [255, 255, 255, 255],
    'white at the visible azimuth',
  );
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, 38),
    [0, 0, 0, 255],
    'black at the adjacent blocked azimuth',
  );
});

// Cycle 5: out-of-window azimuth (bitIndex null) on land → black, not transparent
test('out-of-window azimuth (bitIndex null) on land pixel is black', () => {
  const px = makePixel(true, null);
  assert.deepStrictEqual(
    colorizeBitmaskPixel(px, layout.flagBitIndex, null),
    [0, 0, 0, 255],
  );
});
