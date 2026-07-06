(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.assertSupportedFormatVersion = factory();
  }
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  const SUPPORTED = [1];

  return function assertSupportedFormatVersion(formatVersion) {
    if (typeof formatVersion !== 'number' || !SUPPORTED.includes(formatVersion)) {
      throw new Error(
        `Unsupported tile format_version: ${JSON.stringify(formatVersion)}. ` +
        `Supported: [${SUPPORTED.join(', ')}]. ` +
        'Rebuild the tileset or update the frontend.',
      );
    }
  };
}));
