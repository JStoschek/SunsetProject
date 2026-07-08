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
    static const std::regex pattern(R"(USGS_13_n(\d+)w(\d+)_(\d*).*\.tif)",
                                    std::regex::icase);
    // A tile can appear more than once (USGS re-acquisitions of the same 1°×1°
    // cell, e.g. ..._20100929.tif and ..._20250826.tif).  The newest acquisition
    // date wins; equal dates fall back to the lexicographically greatest
    // filename, so the index never depends on directory-iteration order.
    std::unordered_map<TileKey, std::pair<long, std::string>, PairHash> newest;
    for (const auto& entry : fs::directory_iterator(tile_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::smatch m;
        if (std::regex_match(name, m, pattern)) {
            int lat = std::stoi(m[1].str());
            int lon = std::stoi(m[2].str());
            // Filename tokens are the USGS NW-corner labeling (n{lat}w{lon}).
            const TileKey key = GeoTile::from_usgs(lat, lon);
            // YYYYMMDD acquisition date; a missing date sorts oldest.
            const long date = m[3].length() ? std::stol(m[3].str()) : 0;
            std::pair<long, std::string> rank{date, name};
            auto [it, inserted] = newest.try_emplace(key, rank);
            if (!inserted) {
                if (rank <= it->second) continue;
                it->second = std::move(rank);
            }
            index_[key] = entry.path().string();
        }
    }
}

float DEMTileLoader::get_elevation(double lat, double lon) {
    // Tile identity: the GeoTile owning the coordinate (signed SW corner,
    // klat = floor(lat), klon = floor(lon)).  Compute as ints first and compare
    // directly on the fast path — avoids materializing a GeoTile every call,
    // which showed up at ~5% self-time in profiling (as the former pair did).
    const int klat = (int)std::floor(lat);
    const int klon = (int)std::floor(lon);

    // --- Fast path: same tile as last call (overwhelmingly common during a
    // horizon-sweep march, which steps along one ray inside a single DEM tile
    // for hundreds to thousands of queries in a row).
    const TileData* tile_ptr;
    if (last_tile_ && klat == last_key_.south && klon == last_key_.west) {
        tile_ptr = last_tile_;
    } else {
        const TileKey key{klat, klon};
        auto idx = index_.find(key);
        if (idx == index_.end())
            return std::numeric_limits<float>::quiet_NaN();
        tile_ptr = get_or_load(key, idx->second).get();
        last_key_  = key;
        last_tile_ = tile_ptr;
    }
    return sample_elevation(*tile_ptr, lat, lon);
}

// Read a tile from GDAL into a DEMTile, pre-replacing nodata with 0.0f so the
// sampling hot path needs no per-pixel branch.  Shared by get_or_load (stateful
// LRU path) and freeze (per-strip frozen path).
static DEMTile load_tile(const std::string& path) {
    GDALDataset* ds = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
    if (!ds)
        throw std::runtime_error("Cannot open tile: " + path);

    DEMTile td;
    if (ds->GetGeoTransform(td.gt) != CE_None) {
        GDALClose(ds);
        throw std::runtime_error("Cannot read geotransform: " + path);
    }

    td.width  = ds->GetRasterXSize();
    td.height = ds->GetRasterYSize();

    GDALRasterBand* band = ds->GetRasterBand(1);
    int has_nodata = 0;
    const double nodata_value = band->GetNoDataValue(&has_nodata);

    td.pixels.resize(static_cast<std::size_t>(td.width) * td.height);
    CPLErr err = band->RasterIO(GF_Read, 0, 0, td.width, td.height,
                                 td.pixels.data(), td.width, td.height,
                                 GDT_Float32, 0, 0);
    GDALClose(ds);

    if (err != CE_None)
        throw std::runtime_error("Cannot read tile data: " + path);

    if (has_nodata) {
        const float nd = static_cast<float>(nodata_value);
        for (float& v : td.pixels)
            if (v == nd) v = 0.0f;
    }
    return td;
}

std::shared_ptr<const DEMTileLoader::TileData>
DEMTileLoader::get_or_load(const TileKey& key, const std::string& path)
{
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        return it->second.data;
    }

    // Evict LRU entry if at capacity. Invalidate last_tile_ if it points into
    // the entry we're about to erase.  The shared buffer survives eviction as
    // long as a FrozenDEM still references it.
    if ((int)cache_.size() >= lru_capacity_) {
        if (lru_list_.back() == last_key_)
            last_tile_ = nullptr;
        cache_.erase(lru_list_.back());
        lru_list_.pop_back();
    }

    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.data   = std::make_shared<const TileData>(load_tile(path));
    entry.lru_it = lru_list_.begin();
    return entry.data;
}

FrozenDEM DEMTileLoader::freeze(const std::set<GeoTile>& keys)
{
    FrozenDEM frozen;
    for (const GeoTile key : keys) {
        // GeoTile is one identity regardless of labeling, so the working-set
        // key indexes the loader directly — no convention bridge needed.
        auto idx = index_.find(key);
        if (idx == index_.end())
            continue;  // no tile file for this key — leave it unfrozen
        // Reuses the cached buffer when resident (no reload across strips); the
        // FrozenDEM keeps its own shared_ptr so eviction can't free it mid-strip.
        frozen.insert(key, get_or_load(key, idx->second));
    }
    return frozen;
}
