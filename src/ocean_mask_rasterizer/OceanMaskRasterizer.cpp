#include "OceanMaskRasterizer.h"
#include "FrozenOcean.h"
#include "GshhgWaterPolygonSource.h"
#include "OceanSampling.h"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogr_geometry.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int    kTilePixels = 10800;          // 1° / (1/3″) = 10800
static constexpr double kPixelSize  = 1.0 / kTilePixels;

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------
OceanMaskRasterizer::OceanMaskRasterizer(const std::string& gshhg_full_path,
                                          int lru_capacity)
    : OceanMaskRasterizer(
          std::make_unique<GshhgWaterPolygonSource>(gshhg_full_path),
          lru_capacity)
{}

OceanMaskRasterizer::OceanMaskRasterizer(
    std::unique_ptr<WaterPolygonSource> source, int lru_capacity)
    : source_(std::move(source)), lru_capacity_(lru_capacity)
{
    GDALAllRegister();
    scratch_raster_.resize(static_cast<std::size_t>(kTilePixels) * kTilePixels);
}

// ---------------------------------------------------------------------------
// Rasterize all land polygons that overlap the 1°×1° tile defined by
// (tile_lat, tile_lon) into `raster` (kTilePixels×kTilePixels bytes,
// row-major, north-up).  Initial value 1 = water; burned value 0 = land.
// ---------------------------------------------------------------------------
void OceanMaskRasterizer::rasterize_tile(int tile_lat, int tile_lon,
                                          std::vector<uint8_t>& raster)
{
    // --- Build an in-memory GDAL raster initialised to 1 (water) -----------
    GDALDriver* memDriver =
        GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* ds =
        memDriver->Create("", kTilePixels, kTilePixels, 1, GDT_Byte, nullptr);

    double gt[6] = {
        (double)tile_lon, kPixelSize, 0.0,
        (double)(tile_lat + 1), 0.0, -kPixelSize
    };
    ds->SetGeoTransform(gt);

    GDALRasterBand* band = ds->GetRasterBand(1);
    band->Fill(1.0);   // 1 = water

    // --- Collect land polygons from the source ----------------------------
    std::vector<OGRPolygon*> raw_polys;
    source_->land_polygons_for_tile(tile_lat, tile_lon, raw_polys);

    // Build a tile bounding-box polygon for clipping.  Clipping each polygon
    // to the tile extent before rasterizing avoids passing huge
    // continent-scale polygons (e.g. 1.18 M-point North America) to GDAL
    // when only a small coastal fragment intersects the tile.
    OGRLinearRing* bbox_ring = new OGRLinearRing();
    bbox_ring->addPoint(tile_lon,     tile_lat);
    bbox_ring->addPoint(tile_lon + 1, tile_lat);
    bbox_ring->addPoint(tile_lon + 1, tile_lat + 1);
    bbox_ring->addPoint(tile_lon,     tile_lat + 1);
    bbox_ring->addPoint(tile_lon,     tile_lat);
    OGRPolygon tile_bbox;
    tile_bbox.addRingDirectly(bbox_ring);

    std::vector<OGRGeometryH> hgeoms;
    std::vector<OGRGeometry*> owned_geoms;

    for (OGRPolygon* poly : raw_polys) {
        OGRGeometry* clipped = poly->Intersection(&tile_bbox);
        OGRGeometryFactory::destroyGeometry(poly);
        if (!clipped || clipped->IsEmpty()) {
            if (clipped) OGRGeometryFactory::destroyGeometry(clipped);
            continue;
        }
        hgeoms.push_back(OGRGeometry::ToHandle(clipped));
        owned_geoms.push_back(clipped);
    }

    // --- Rasterize: burn value 0 = land ------------------------------------
    if (!hgeoms.empty()) {
        int bandList[] = {1};
        std::vector<double> burnValues(hgeoms.size(), 0.0);
        GDALRasterizeGeometries(
            ds,
            1, bandList,
            (int)hgeoms.size(), hgeoms.data(),
            nullptr, nullptr,
            burnValues.data(),
            nullptr,
            nullptr, nullptr);
    }

    // --- Read the raster back into the caller's buffer ---------------------
    band->RasterIO(GF_Read,
                   0, 0, kTilePixels, kTilePixels,
                   raster.data(), kTilePixels, kTilePixels,
                   GDT_Byte, 0, 0);

    GDALClose(ds);
    for (OGRGeometry* g : owned_geoms)
        OGRGeometryFactory::destroyGeometry(g);
}

// ---------------------------------------------------------------------------
static std::vector<uint64_t> pack_raster(const std::vector<uint8_t>& raster)
{
    const int total = kTilePixels * kTilePixels;
    std::vector<uint64_t> bits((total + 63) / 64, 0ULL);
    for (int i = 0; i < total; ++i) {
        if (raster[i] != 0)
            bits[i / 64] |= (uint64_t(1) << (i % 64));
    }
    return bits;
}

// ---------------------------------------------------------------------------
std::shared_ptr<const std::vector<uint64_t>>
OceanMaskRasterizer::get_or_rasterize(int tile_lat, int tile_lon)
{
    const TileKey key{tile_lat, tile_lon};

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        last_key_  = key;
        last_bits_ = it->second.bits->data();
        return it->second.bits;
    }

    if ((int)cache_.size() >= lru_capacity_) {
        if (lru_list_.back() == last_key_)
            last_bits_ = nullptr;
        cache_.erase(lru_list_.back());
        lru_list_.pop_back();
    }

    rasterize_tile(tile_lat, tile_lon, scratch_raster_);
    ++rasterize_count_;

    auto bits = std::make_shared<const std::vector<uint64_t>>(
        pack_raster(scratch_raster_));
    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.bits   = bits;
    entry.lru_it = lru_list_.begin();

    last_key_  = key;
    last_bits_ = bits->data();
    return bits;
}

bool OceanMaskRasterizer::is_water(double lat, double lon)
{
    const int tile_lat = (int)std::floor(lat);
    const int tile_lon = (int)std::floor(lon);
    const TileKey key{tile_lat, tile_lon};

    if (last_bits_ && key == last_key_)
        return sample_water(last_bits_, tile_lat, tile_lon, lat, lon);

    get_or_rasterize(tile_lat, tile_lon);
    return sample_water(last_bits_, tile_lat, tile_lon, lat, lon);
}

FrozenOcean OceanMaskRasterizer::freeze(
    const std::set<std::pair<int, int>>& geo_keys)
{
    FrozenOcean frozen;
    for (const auto& [tile_lat, tile_lon] : geo_keys)
        frozen.insert({tile_lat, tile_lon}, get_or_rasterize(tile_lat, tile_lon));
    return frozen;
}

OceanOriginResult
OceanMaskRasterizer::ocean_origin_for_ray(double azimuth_deg,
                                           double lat, double lon,
                                           double step_km, double max_km)
{
    return march_to_coast(azimuth_deg, lat, lon, step_km, max_km,
                          [this](double la, double lo) { return is_water(la, lo); });
}
