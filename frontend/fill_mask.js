// Rasterize the us_land_fill polygon onto Visibility Tile pixel grids.
//
// The "non-visible outside the boxes" black is painted by the SAME raster
// layer as the computed tiles: real tiles fill their transparent margins from
// this mask, and tiles that don't exist in the pyramid are synthesized from
// it. Because everything lands on one pixel grid in one layer, the box-edge
// seams that two stacked translucent layers produce (overlap → darker line,
// gap → lighter line) cannot exist: black ∪ black is just black.
//
// Geometry is converted once to unit Web-Mercator (the tile grid's space),
// then each tile row is filled by even–odd scanline over the edge soup of all
// rings — holes (ocean, Processing Boxes) fall out of the parity naturally.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.createFillMasker = factory();
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  // lon/lat → unit Web-Mercator [0,1]².
  function unitX(lng) { return (lng + 180) / 360; }
  function unitY(lat) {
    const s = Math.sin((lat * Math.PI) / 180);
    return 0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI);
  }

  return function createFillMasker(multiPolygon) {
    // Flatten every ring (outers and holes alike) into one edge soup.
    // Horizontal edges are dropped — they can never cross a scanline.
    const e = [];
    for (const poly of multiPolygon) {
      for (const ring of poly) {
        for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
          const ax = unitX(ring[j][0]), ay = unitY(ring[j][1]);
          const bx = unitX(ring[i][0]), by = unitY(ring[i][1]);
          if (ay !== by) e.push(ax, ay, bx, by);
        }
      }
    }
    const edgeCount = e.length / 4;
    const E = Float64Array.from(e);

    // The mask for one XYZ tile: Uint8Array(ts*ts), 1 = inside the fill.
    // Returns null when the tile is entirely outside (the common case over
    // ocean and abroad), so callers can keep their cheap empty-tile path.
    return function maskForTile(z, x, y, ts) {
      const scale = 1 / Math.pow(2, z);
      const tx0 = x * scale;
      const ty0 = y * scale;

      // Edges overlapping this tile's y-band. The x extent is deliberately
      // unlimited: a row's parity depends on every crossing to its left, no
      // matter how far outside the tile it lies.
      const band = [];
      const yLo = ty0, yHi = ty0 + scale;
      for (let i = 0; i < edgeCount; i++) {
        const y0 = E[i * 4 + 1], y1 = E[i * 4 + 3];
        if (Math.max(y0, y1) > yLo && Math.min(y0, y1) < yHi) band.push(i);
      }
      if (band.length === 0) return null; // no crossings at these rows → outside

      const mask = new Uint8Array(ts * ts);
      const xs = [];
      let any = false;
      for (let r = 0; r < ts; r++) {
        const yy = ty0 + ((r + 0.5) / ts) * scale;
        xs.length = 0;
        for (const i of band) {
          const y0 = E[i * 4 + 1], y1 = E[i * 4 + 3];
          if ((y0 > yy) !== (y1 > yy)) {
            const t = (yy - y0) / (y1 - y0);
            xs.push(E[i * 4] + t * (E[i * 4 + 2] - E[i * 4]));
          }
        }
        if (xs.length < 2) continue;
        xs.sort((a, b) => a - b);
        const rowOff = r * ts;
        for (let k = 0; k + 1 < xs.length; k += 2) {
          // Pixel-centre columns covered by the inside span [xs[k], xs[k+1]].
          let c0 = Math.ceil(((xs[k] - tx0) / scale) * ts - 0.5);
          let c1 = Math.floor(((xs[k + 1] - tx0) / scale) * ts - 0.5);
          if (c1 < 0 || c0 >= ts) continue;
          if (c0 < 0) c0 = 0;
          if (c1 >= ts) c1 = ts - 1;
          mask.fill(1, rowOff + c0, rowOff + c1 + 1);
          any = true;
        }
      }
      return any ? mask : null;
    };
  };
}));
