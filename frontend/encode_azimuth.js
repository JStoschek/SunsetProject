(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.encodeAzimuth = factory();
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  return function encodeAzimuth(compassAz) {
    if (compassAz < 200 || compassAz > 360) {
      console.warn('encodeAzimuth: compassAz ' + compassAz + ' is outside the 200–360° window; clamping');
      compassAz = Math.max(200, Math.min(360, compassAz));
    }
    return Math.round((compassAz - 200) / 160 * 255);
  };
}));
