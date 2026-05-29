'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const encodeAzimuth = require('../encode_azimuth.js');

// Slice 1: core formula — 270° maps to byte 112.
// round((270 - 200) / 160 * 255) = round(70/160 * 255) = round(111.5625) = 112
test('270° encodes to byte 112', () => {
  assert.strictEqual(encodeAzimuth(270), 112);
});

// Slice 2: lower boundary — 200° is the minimum window edge → 0.
test('200° encodes to byte 0', () => {
  assert.strictEqual(encodeAzimuth(200), 0);
});

// Slice 3: upper boundary — 360° is the maximum window edge → 255.
test('360° encodes to byte 255', () => {
  assert.strictEqual(encodeAzimuth(360), 255);
});

// Slice 4: input below window — warns and clamps to 200 (byte 0), never negative.
test('azimuth below 200 warns and returns 0, never negative', () => {
  const warnings = [];
  const orig = console.warn;
  console.warn = (...args) => warnings.push(args.join(' '));
  try {
    const result = encodeAzimuth(100);
    assert.ok(warnings.length >= 1, 'expected console.warn to fire');
    assert.strictEqual(result, 0);
  } finally {
    console.warn = orig;
  }
});

// Slice 5: input above window — warns and clamps to 360 (byte 255), never > 255.
test('azimuth above 360 warns and returns 255, never > 255', () => {
  const warnings = [];
  const orig = console.warn;
  console.warn = (...args) => warnings.push(args.join(' '));
  try {
    const result = encodeAzimuth(400);
    assert.ok(warnings.length >= 1, 'expected console.warn to fire');
    assert.strictEqual(result, 255);
  } finally {
    console.warn = orig;
  }
});
