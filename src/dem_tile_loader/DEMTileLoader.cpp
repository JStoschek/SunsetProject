#include "DEMTileLoader.h"
#include <gdal_priv.h>
#include <filesystem>
#include <limits>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;

DEMTileLoader::DEMTileLoader(const std::string& tile_dir, int lru_capacity)
    : lru_capacity_(lru_capacity)
{
    GDALAllRegister();
    static const std::regex pattern(R"(USGS_13_n(\d+)w(\d+)_.*\.tif)",
                                    std::regex::icase);
    for (const auto& entry : fs::directory_iterator(tile_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::smatch m;
        if (std::regex_match(name, m, pattern)) {
            int lat = std::stoi(m[1].str());
            int lon = std::stoi(m[2].str());
            index_[{lat, lon}] = entry.path().string();
        }
    }
}

float DEMTileLoader::get_elevation(double lat, double lon) {
    // Tile key: NW corner (lat_n = floor(lat)+1, lon_w = ceil(|lon|)).
    // Compute as ints first and compare directly on the fast path — avoids
    // constructing a std::pair<int,int> every call, which showed up at ~5%
    // self-time in profiling.
    const int klat = (int)std::floor(lat) + 1;
    const int klon = (int)std::ceil(std::fabs(lon));

    // --- Fast path: same tile as last call (overwhelmingly common during a
    // horizon-sweep march, which steps along one ray inside a single DEM tile
    // for hundreds to thousands of queries in a row).
    const TileData* tile_ptr;
    if (last_tile_ && klat == last_key_.first && klon == last_key_.second) {
        tile_ptr = last_tile_;
    } else {
        const TileKey key{klat, klon};
        auto idx = index_.find(key);
        if (idx == index_.end())
            return std::numeric_limits<float>::quiet_NaN();
        tile_ptr = &get_or_load(key, idx->second);
        last_key_  = key;
        last_tile_ = tile_ptr;
    }
    const TileData& tile = *tile_ptr;

    // Raw pixel coordinates: 0 = left/top edge of first pixel; centre of pixel N is at N+0.5.
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
    // Clamp so the right/bottom neighbour never exceeds the tile extent.
    col0 = std::max(col0, 0);
    row0 = std::max(row0, 0);
    int col1 = std::min(col0 + 1, tile.width  - 1);
    int row1 = std::min(row0 + 1, tile.height - 1);

    double tx = px - col0;
    double ty = py - row0;

    // Nodata was pre-replaced with 0.0f at load time (see get_or_load), so this
    // is a plain indexed load — no branch per pixel.
    const float* px_data = tile.pixels.data();
    const int W = tile.width;
    float p00 = px_data[row0 * W + col0];
    float p10 = px_data[row0 * W + col1];
    float p01 = px_data[row1 * W + col0];
    float p11 = px_data[row1 * W + col1];

    return (float)((1.0 - ty) * ((1.0 - tx) * p00 + tx * p10) +
                          ty  * ((1.0 - tx) * p01 + tx * p11));
}

const DEMTileLoader::TileData& DEMTileLoader::get_or_load(const TileKey& key,
                                                           const std::string& path)
{
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        return it->second.data;
    }

    // Evict LRU entry if at capacity. Invalidate last_tile_ if it points into
    // the entry we're about to erase.
    if ((int)cache_.size() >= lru_capacity_) {
        if (lru_list_.back() == last_key_)
            last_tile_ = nullptr;
        cache_.erase(lru_list_.back());
        lru_list_.pop_back();
    }

    // Load via GDAL
    GDALDataset* ds = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
    if (!ds)
        throw std::runtime_error("Cannot open tile: " + path);

    TileData td;
    if (ds->GetGeoTransform(td.gt) != CE_None) {
        GDALClose(ds);
        throw std::runtime_error("Cannot read geotransform: " + path);
    }

    td.width  = ds->GetRasterXSize();
    td.height = ds->GetRasterYSize();

    GDALRasterBand* band = ds->GetRasterBand(1);
    int has_nodata = 0;
    td.nodata_value = band->GetNoDataValue(&has_nodata);
    td.has_nodata   = (has_nodata != 0);

    td.pixels.resize(td.width * td.height);
    CPLErr err = band->RasterIO(GF_Read, 0, 0, td.width, td.height,
                                 td.pixels.data(), td.width, td.height,
                                 GDT_Float32, 0, 0);
    GDALClose(ds);

    if (err != CE_None)
        throw std::runtime_error("Cannot read tile data: " + path);

    // Pre-replace nodata with 0.0 once at load time, so get_elevation's hot
    // inner loop can do a plain indexed load with no branch per pixel.
    if (td.has_nodata) {
        const float nd = static_cast<float>(td.nodata_value);
        for (float& v : td.pixels) {
            if (v == nd) v = 0.0f;
        }
        td.has_nodata = false;
    }

    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.data   = std::move(td);
    entry.lru_it = lru_list_.begin();
    return entry.data;
}
