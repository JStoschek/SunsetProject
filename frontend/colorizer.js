(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.colorizeBitmaskPixel = factory();
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  function getBit(bytes, bitIndex) {
    return (bytes[bitIndex >> 3] & (1 << (bitIndex & 7))) !== 0;
  }

  return function colorizeBitmaskPixel(pixelBytes, flagBitIndex, bitIndex) {
    if (!getBit(pixelBytes, flagBitIndex)) return [0, 0, 0, 0];
    if (bitIndex !== null && getBit(pixelBytes, bitIndex)) return [255, 255, 255, 255];
    return [0, 0, 0, 255];
  };
}));
