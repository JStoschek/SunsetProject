// Pure (date, latitude) → compassAzimuthDegrees using SunCalc.
// UMD: browser <script> reads globalThis.SunCalc; Node require() loads suncalc npm package.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory(require('suncalc'));
  } else {
    root.sunsetAzimuth = factory(root.SunCalc);
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function (SunCalc) {
  // lon is a nominal Bay Area value; azimuth at sunset is determined by lat+date,
  // not lon, so any longitude in the same hemisphere gives the same compass bearing.
  var LON = -122.5;

  return function sunsetAzimuth(year, month, day, lat) {
    // Local noon guarantees the correct calendar date in any host timezone.
    var localNoon = new Date(year, month - 1, day, 12, 0, 0);
    var times = SunCalc.getTimes(localNoon, lat, LON);
    var pos = SunCalc.getPosition(times.sunset, lat, LON);
    // SunCalc azimuth: radians from south (south=0, west=+π/2, east=-π/2).
    // Convert to compass bearing: add 180°, normalize to [0, 360).
    var compassAz = ((pos.azimuth * 180 / Math.PI + 180) % 360 + 360) % 360;
    return compassAz;
  };
}));
