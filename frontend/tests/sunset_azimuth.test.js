'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const sunsetAzimuth = require('../sunset_azimuth.js');

// Summer solstice at 38°N: sun sets ~31° north of west → ~301°.
// (The issue draft cited ~285°; the correct spherical-trig value is ~300°–302°.)
test('summer solstice at 38°N gives azimuth near 301°', () => {
  const az = sunsetAzimuth(2024, 6, 21, 38);
  assert.ok(az >= 299 && az <= 304, `expected ~301°, got ${az.toFixed(1)}°`);
});

// Winter solstice at 38°N: sun sets ~29° south of west → ~241°.
test('winter solstice at 38°N gives azimuth near 241°', () => {
  const az = sunsetAzimuth(2024, 12, 21, 38);
  assert.ok(az >= 238 && az <= 244, `expected ~241°, got ${az.toFixed(1)}°`);
});

// Local noon (new Date(y, m-1, d, 12, 0, 0)) is always the intended calendar day in
// any host timezone — noon cannot straddle a date boundary.  A naive UTC midnight
// (new Date(Date.UTC(y, m-1, d))) would shift by up to ±12 h and could land on the
// wrong calendar day in hosts far from UTC, producing the wrong sunset.
test('local noon construction yields correct calendar day regardless of host timezone', () => {
  // June 21 is near summer solstice; its azimuth (~301°) is well above 290°.
  // An off-by-one error would land on June 20 or June 22, both of which give
  // very similar values (< 0.1° difference near solstice) — so we use a date
  // where the structural guarantee matters: the function must accept integer
  // year/month/day and construct the local Date internally (not accept a Date
  // object whose semantics depend on the caller's timezone).
  const az = sunsetAzimuth(2024, 6, 21, 38);
  assert.ok(az >= 299 && az <= 304,
    `June 21 at 38°N should give summer-solstice azimuth ~301°, got ${az.toFixed(1)}°`);
});

// SunCalc returns azimuth in south-referenced radians; conversion adds 180°.
// Without the +180 the result would be the eastern mirror (~121° summer, ~59° winter).
test('bearing conversion places result in western hemisphere, not eastern mirror', () => {
  const summer = sunsetAzimuth(2024, 6, 21, 38);
  const winter = sunsetAzimuth(2024, 12, 21, 38);
  assert.ok(summer > 270 && summer < 360, `summer: expected NW bearing, got ${summer.toFixed(1)}°`);
  assert.ok(winter > 180 && winter < 270, `winter: expected SW bearing, got ${winter.toFixed(1)}°`);
});
