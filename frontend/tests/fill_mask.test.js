'use strict';
const test = require('node:test');
const assert = require('node:assert');
const createFillMasker = require('../fill_mask.js');

// A latitude whose unit-mercator y is exactly 0.25 (asinh(tan φ) = π/2), so
// test polygons line up exactly with power-of-two tile boundaries.
const LAT_Y025 = (Math.atan(Math.sinh(Math.PI / 2)) * 180) / Math.PI;

// Square covering unit-mercator x ∈ [0.25, 0.5], y ∈ [0.25, 0.5]:
// lon ∈ [-90, 0], lat ∈ [0, LAT_Y025] — exactly tile z2/(1,1).
const SQUARE = [[[
  [-90, 0], [0, 0], [0, LAT_Y025], [-90, LAT_Y025], [-90, 0],
]]];

test('tile fully inside the polygon is all ones', () => {
  const mask = createFillMasker(SQUARE)(2, 1, 1, 8);
  assert.ok(mask, 'expected a mask');
  assert.ok(mask.every((v) => v === 1));
});

test('tile fully outside the polygon is null', () => {
  const masker = createFillMasker(SQUARE);
  assert.strictEqual(masker(2, 3, 3, 8), null); // far corner
  assert.strictEqual(masker(2, 0, 0, 8), null); // adjacent, above-left
});

test('straddling tile covers exactly the pixels whose centre is inside', () => {
  // Tile z1/(0,0) spans x,y ∈ [0, 0.5]: the polygon is its bottom-right
  // quadrant, so with ts=8 exactly columns/rows 4..7 are inside.
  const mask = createFillMasker(SQUARE)(1, 0, 0, 8);
  assert.ok(mask, 'expected a mask');
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const want = r >= 4 && c >= 4 ? 1 : 0;
      assert.strictEqual(mask[r * 8 + c], want, `pixel (${r},${c})`);
    }
  }
});

test('holes are excluded by even-odd parity', () => {
  // Same square with a hole over its centre: the hole spans the middle half
  // of tile z2/(1,1) in both axes → with ts=8, rows/cols 2..5 are the hole.
  const holeWest = -90 + 90 * 0.25, holeEast = -90 + 90 * 0.75;
  // lat bounds of the hole: unit-y 0.3125 and 0.4375 via inverse mercator.
  const latOfY = (y) => (Math.atan(Math.sinh((0.5 - y) * 2 * Math.PI)) * 180) / Math.PI;
  const withHole = [[
    SQUARE[0][0],
    [
      [holeWest, latOfY(0.4375)], [holeEast, latOfY(0.4375)],
      [holeEast, latOfY(0.3125)], [holeWest, latOfY(0.3125)],
      [holeWest, latOfY(0.4375)],
    ],
  ]];
  const mask = createFillMasker(withHole)(2, 1, 1, 8);
  assert.ok(mask, 'expected a mask');
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const inHole = r >= 2 && r <= 5 && c >= 2 && c <= 5;
      assert.strictEqual(mask[r * 8 + c], inHole ? 0 : 1, `pixel (${r},${c})`);
    }
  }
});

test('a mask with no covered pixels collapses to null', () => {
  // The tile row-band intersects the polygon's y-range, but every inside
  // span lies left of the tile → no pixel covered → null, not all-zeros.
  const mask = createFillMasker(SQUARE)(2, 3, 1, 8);
  assert.strictEqual(mask, null);
});
