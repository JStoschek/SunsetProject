'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const assertSupportedFormatVersion = require('../format_guard.js');

test('format_version 1 is accepted without throwing', () => {
  assert.doesNotThrow(() => assertSupportedFormatVersion(1));
});

test('format_version 2 throws with a descriptive error mentioning the bad version', () => {
  assert.throws(
    () => assertSupportedFormatVersion(2),
    (err) => {
      assert.ok(err instanceof Error);
      assert.ok(
        err.message.includes('2'),
        `error message should mention the bad version, got: "${err.message}"`,
      );
      return true;
    },
  );
});

test('format_version 0 throws', () => {
  assert.throws(() => assertSupportedFormatVersion(0));
});

test('format_version string "1" throws (type mismatch)', () => {
  assert.throws(() => assertSupportedFormatVersion('1'));
});
