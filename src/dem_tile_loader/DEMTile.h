#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// An immutable 1°×1° elevation tile: a float pixel buffer plus its
// geotransform.  Shared read-only between the stateful DEMTileLoader cache and
// the frozen per-strip cache (FrozenDEM).  Nodata is pre-replaced with 0.0f at
// load time, so sampling needs no per-pixel branch.
struct DEMTile {
    std::vector<float> pixels;
    int    width  = 0;
    int    height = 0;
    double gt[6]  = {};
};

// Bilinear-sample elevation at (lat, lon) within `tile`.  Returns NaN when the
// coordinate falls outside the tile's pixel extent.  This is the single sampling
// routine shared by DEMTileLoader::get_elevation and FrozenDEMView, so frozen
// reads are identical to the stateful loader's by construction.
inline float sample_elevation(const DEMTile& tile, double lat, double lon) {
    // Raw pixel coordinates: 0 = left/top edge of first pixel; centre of pixel N
    // is at N+0.5.
    double raw_col = (lon - tile.gt[0]) / tile.gt[1];
    double raw_row = (lat - tile.gt[3]) / tile.gt[5];

    int col = (int)std::floor(raw_col);
    int row = (int)std::floor(raw_row);
    if (col < 0 || col >= tile.width || row < 0 || row >= tile.height)
        return std::numeric_limits<float>::quiet_NaN();

    // Continuous coordinate in pixel-centre space (centre of pixel N = N).
    double px = raw_col - 0.5;
    double py = raw_row - 0.5;

    int col0 = (int)std::floor(px);
    int row0 = (int)std::floor(py);
    col0 = std::max(col0, 0);
    row0 = std::max(row0, 0);
    int col1 = std::min(col0 + 1, tile.width  - 1);
    int row1 = std::min(row0 + 1, tile.height - 1);

    double tx = px - col0;
    double ty = py - row0;

    const float* data = tile.pixels.data();
    const int W = tile.width;
    float p00 = data[row0 * W + col0];
    float p10 = data[row0 * W + col1];
    float p01 = data[row1 * W + col0];
    float p11 = data[row1 * W + col1];

    return (float)((1.0 - ty) * ((1.0 - tx) * p00 + tx * p10) +
                          ty  * ((1.0 - tx) * p01 + tx * p11));
}
