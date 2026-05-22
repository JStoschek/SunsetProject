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
    // Tile key: NW corner (lat_n = floor(lat)+1, lon_w = ceil(|lon|))
    auto key = TileKey{(int)std::floor(lat) + 1, (int)std::ceil(std::fabs(lon))};

    auto idx = index_.find(key);
    if (idx == index_.end())
        return std::numeric_limits<float>::quiet_NaN();

    const TileData& tile = get_or_load(key, idx->second);

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

    auto sample = [&](int c, int r) -> float {
        float v = tile.pixels[r * tile.width + c];
        return (tile.has_nodata && v == (float)tile.nodata_value) ? 0.0f : v;
    };

    float p00 = sample(col0, row0);
    float p10 = sample(col1, row0);
    float p01 = sample(col0, row1);
    float p11 = sample(col1, row1);

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

    // Evict LRU entry if at capacity
    if ((int)cache_.size() >= lru_capacity_) {
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

    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.data   = std::move(td);
    entry.lru_it = lru_list_.begin();
    return entry.data;
}
