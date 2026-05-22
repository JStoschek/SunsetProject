#pragma once
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class DEMTileLoader {
public:
    explicit DEMTileLoader(const std::string& tile_dir, int lru_capacity = 8);
    float get_elevation(double lat, double lon);
    std::size_t tile_count() const { return index_.size(); }

private:
    struct PairHash {
        std::size_t operator()(std::pair<int, int> p) const noexcept {
            return std::hash<long long>{}((long long)p.first << 32 | (unsigned int)p.second);
        }
    };

    using TileKey = std::pair<int, int>;

    struct TileData {
        std::vector<float> pixels;
        int width  = 0;
        int height = 0;
        double gt[6] = {};
        double nodata_value = 0.0;
        bool has_nodata = false;
    };

    struct CacheEntry {
        TileData data;
        std::list<TileKey>::iterator lru_it;
    };

    std::unordered_map<TileKey, std::string, PairHash>    index_;
    std::unordered_map<TileKey, CacheEntry, PairHash>     cache_;
    std::list<TileKey>                                     lru_list_;
    int lru_capacity_;

    const TileData& get_or_load(const TileKey& key, const std::string& path);
};
