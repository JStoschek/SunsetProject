#include "OceanMaskRasterizer.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogr_geometry.h>

// ---------------------------------------------------------------------------
// GSHHG binary files are big-endian.  Byte-swap a single int32_t.
// ---------------------------------------------------------------------------
static int32_t be32(int32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    std::memcpy(&v, &u, 4);
#endif
    return v;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int    kTilePixels = 10800;          // 1° / (1/3″) = 10800
static constexpr double kPixelSize  = 1.0 / kTilePixels;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
OceanMaskRasterizer::OceanMaskRasterizer(const std::string& gshhg_full_path,
                                          int lru_capacity)
    : lru_capacity_(lru_capacity), path_(gshhg_full_path)
{
    file_.open(gshhg_full_path, std::ios::binary);
    if (!file_.is_open())
        throw std::runtime_error(
            "OceanMaskRasterizer: cannot open GSHHG file: " + gshhg_full_path);

    // Validate by reading the first polygon header.
    GshhgHeader hdr{};
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!file_)
        throw std::runtime_error(
            "OceanMaskRasterizer: file too short to contain a GSHHG header: "
            + gshhg_full_path);

    // Byte-swap from big-endian.
    hdr.id   = be32(hdr.id);
    hdr.n    = be32(hdr.n);
    hdr.flag = be32(hdr.flag);

    // First polygon must be id=0, have at least 1 point, and be level 1 (land).
    int level = hdr.flag & 0xFF;
    if (hdr.id != 0 || hdr.n < 1 || level != 1)
        throw std::runtime_error(
            "OceanMaskRasterizer: file does not look like a GSHHG binary: "
            + gshhg_full_path);

    // Rewind to start for the index build pass.
    file_.seekg(0, std::ios::beg);

    GDALAllRegister();

    // -----------------------------------------------------------------------
    // Build spatial index: scan every polygon header (skip point data) and
    // record a PolyRef for each Level-1 polygon in every 1°×1° tile its
    // bounding box overlaps.
    // -----------------------------------------------------------------------
    auto microdeg_to_lon = [](int32_t v) -> double {
        double d = v * 1e-6;
        return (d > 180.0) ? d - 360.0 : d;
    };

    while (file_) {
        const std::streamoff hdr_offset = file_.tellg();

        GshhgHeader hdr{};
        file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!file_) break;

        hdr.id        = be32(hdr.id);
        hdr.n         = be32(hdr.n);
        hdr.flag      = be32(hdr.flag);
        hdr.west      = be32(hdr.west);
        hdr.east      = be32(hdr.east);
        hdr.south     = be32(hdr.south);
        hdr.north     = be32(hdr.north);
        hdr.area      = be32(hdr.area);
        hdr.area_full = be32(hdr.area_full);
        hdr.container = be32(hdr.container);
        hdr.ancestor  = be32(hdr.ancestor);

        const int level = hdr.flag & 0xFF;
        const int n     = hdr.n;

        // Skip point data — we only need the header for the index.
        file_.seekg(static_cast<std::streamoff>(sizeof(GshhgPoint)) * n,
                    std::ios::cur);

        if (level != 1) continue;

        // Convert bounding box to degrees ([-180, 180] convention).
        const double poly_west  = microdeg_to_lon(hdr.west);
        const double poly_east  = microdeg_to_lon(hdr.east);
        const double poly_south = hdr.south * 1e-6;
        const double poly_north = hdr.north * 1e-6;

        // Enumerate every 1°×1° tile that overlaps this polygon's AABB.
        const int tile_lon_min = (int)std::floor(poly_west);
        const int tile_lon_max = (int)std::floor(poly_east - 1e-9);
        const int tile_lat_min = (int)std::floor(poly_south);
        const int tile_lat_max = (int)std::floor(poly_north - 1e-9);

        for (int tlat = tile_lat_min; tlat <= tile_lat_max; ++tlat) {
            for (int tlon = tile_lon_min; tlon <= tile_lon_max; ++tlon) {
                index_[{tlat, tlon}].push_back({hdr_offset, n});
            }
        }
    }

    // Clear stream state so is_water() can seek freely.
    file_.clear();
}

// ---------------------------------------------------------------------------
// Stubs — full implementations in later issues.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Rasterize all GSHHG Level-1 land polygons that overlap the 1°×1° tile
// defined by (tile_lat, tile_lon) into `raster` (kTilePixels×kTilePixels
// bytes, row-major, north-up).  Initial value 1 = water; burned value 0 = land.
//
// Uses the pre-built spatial index (index_) to seek directly to each polygon's
// header instead of scanning the entire file from byte 0.
// ---------------------------------------------------------------------------
void OceanMaskRasterizer::rasterize_tile(int tile_lat, int tile_lon,
                                          std::vector<uint8_t>& raster)
{
    // --- Build an in-memory GDAL raster initialised to 1 (water) -----------
    GDALDriver* memDriver =
        GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* ds =
        memDriver->Create("", kTilePixels, kTilePixels, 1, GDT_Byte, nullptr);

    // North-up geotransform: upper-left corner = (tile_lon, tile_lat+1).
    double gt[6] = {
        (double)tile_lon, kPixelSize, 0.0,
        (double)(tile_lat + 1), 0.0, -kPixelSize
    };
    ds->SetGeoTransform(gt);

    GDALRasterBand* band = ds->GetRasterBand(1);
    band->Fill(1.0);   // 1 = water

    // --- Collect Level-1 polygons via the spatial index --------------------
    auto microdeg_to_lon = [](int32_t v) -> double {
        double d = v * 1e-6;
        return (d > 180.0) ? d - 360.0 : d;
    };

    std::vector<OGRGeometryH> hgeoms;
    std::vector<OGRGeometry*> owned_geoms;  // for cleanup

    // Build a tile bounding-box polygon once for clipping.
    // Clipping each polygon to the tile extent before rasterizing avoids
    // passing huge continent-scale polygons (e.g. 1.18 M-point North America)
    // to GDAL when only a small coastal fragment intersects the tile.
    OGRLinearRing* bbox_ring = new OGRLinearRing();
    bbox_ring->addPoint(tile_lon,     tile_lat);
    bbox_ring->addPoint(tile_lon + 1, tile_lat);
    bbox_ring->addPoint(tile_lon + 1, tile_lat + 1);
    bbox_ring->addPoint(tile_lon,     tile_lat + 1);
    bbox_ring->addPoint(tile_lon,     tile_lat);
    OGRPolygon tile_bbox;
    tile_bbox.addRingDirectly(bbox_ring);

    const TileKey key{tile_lat, tile_lon};
    auto idx_it = index_.find(key);
    if (idx_it != index_.end()) {
        for (const PolyRef& ref : idx_it->second) {
            // Seek to the polygon's header.
            file_.clear();
            file_.seekg(ref.offset, std::ios::beg);

            GshhgHeader hdr{};
            file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
            if (!file_) continue;

            const int n = ref.n;

            // Read polygon points and build an OGRPolygon.
            std::vector<GshhgPoint> pts(n);
            file_.read(reinterpret_cast<char*>(pts.data()),
                       static_cast<std::streamsize>(sizeof(GshhgPoint)) * n);
            if (!file_) continue;

            OGRLinearRing* ring = new OGRLinearRing();
            ring->setNumPoints(n + 1);
            for (int i = 0; i < n; ++i) {
                double lon = microdeg_to_lon(be32(pts[i].x));
                double lat = be32(pts[i].y) * 1e-6;
                ring->setPoint(i, lon, lat);
            }
            // Close the ring.
            ring->setPoint(n, ring->getX(0), ring->getY(0));

            OGRPolygon* poly = new OGRPolygon();
            poly->addRingDirectly(ring);

            // Clip to tile extent.  Without this, continent-scale polygons
            // (e.g. North America with 1.18 M points) force GDAL to check
            // every edge even when no pixels in this tile are covered.
            OGRGeometry* clipped = poly->Intersection(&tile_bbox);
            OGRGeometryFactory::destroyGeometry(poly);
            if (!clipped || clipped->IsEmpty()) {
                if (clipped) OGRGeometryFactory::destroyGeometry(clipped);
                continue;
            }

            hgeoms.push_back(OGRGeometry::ToHandle(clipped));
            owned_geoms.push_back(clipped);
        }
    }
    // If key not in index_, the tile has no overlapping Level-1 polygons
    // (pure ocean) — leave the raster all-water.

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

    // Cleanup.
    GDALClose(ds);
    for (OGRGeometry* g : owned_geoms)
        OGRGeometryFactory::destroyGeometry(g);
}

// ---------------------------------------------------------------------------
// Pack a kTilePixels×kTilePixels byte raster (1=water, 0=land) into a
// compact 1-bit-per-pixel array of uint64_t words.
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
bool OceanMaskRasterizer::is_water(double lat, double lon)
{
    const int tile_lat = (int)std::floor(lat);
    const int tile_lon = (int)std::floor(lon);
    const TileKey key{tile_lat, tile_lon};

    // Pixel coordinates within the tile (clamp to valid range).
    int col = (int)((lon - tile_lon) * kTilePixels);
    int row = (int)((tile_lat + 1.0 - lat) * kTilePixels);
    col = std::max(0, std::min(col, kTilePixels - 1));
    row = std::max(0, std::min(row, kTilePixels - 1));
    const int idx = row * kTilePixels + col;

    // --- Cache lookup ---
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        // Cache hit: promote to most-recently-used.
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        const auto& bits = it->second.bits;
        return (bits[idx / 64] >> (idx % 64)) & 1ULL;
    }

    // --- Cache miss: evict LRU if at capacity, then rasterize and insert ---
    if ((int)cache_.size() >= lru_capacity_) {
        cache_.erase(lru_list_.back());
        lru_list_.pop_back();
    }

    std::vector<uint8_t> raster(kTilePixels * kTilePixels);
    rasterize_tile(tile_lat, tile_lon, raster);
    ++rasterize_count_;

    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.bits   = pack_raster(raster);
    entry.lru_it = lru_list_.begin();

    const auto& bits = entry.bits;
    return (bits[idx / 64] >> (idx % 64)) & 1ULL;
}

// ---------------------------------------------------------------------------
// Spherical-Earth helper: destination point given start (lat, lon in degrees),
// bearing (degrees clockwise from north), and distance (km).
// Uses the haversine forward formula with R = 6371 km.
// ---------------------------------------------------------------------------
static std::pair<double,double> geo_destination(double lat, double lon,
                                                  double bearing_deg, double dist_km)
{
    const double R     = 6371.0;
    const double d     = dist_km / R;
    const double lat1  = lat         * M_PI / 180.0;
    const double lon1  = lon         * M_PI / 180.0;
    const double theta = bearing_deg * M_PI / 180.0;

    const double lat2 = std::asin(std::sin(lat1) * std::cos(d)
                                  + std::cos(lat1) * std::sin(d) * std::cos(theta));
    const double lon2 = lon1 + std::atan2(std::sin(theta) * std::sin(d) * std::cos(lat1),
                                           std::cos(d) - std::sin(lat1) * std::sin(lat2));

    return {lat2 * 180.0 / M_PI, lon2 * 180.0 / M_PI};
}

OceanOriginResult
OceanMaskRasterizer::ocean_origin_for_ray(double azimuth_deg,
                                           double lat, double lon,
                                           double step_km, double max_km)
{
    // March along azimuth_deg (toward the coast) until hitting land.
    const double offset_km = 200.0;

    double crossing_lat = lat;
    double crossing_lon = lon;
    bool   found        = false;

    for (double dist = step_km; dist <= max_km; dist += step_km) {
        auto [pt_lat, pt_lon] = geo_destination(lat, lon, azimuth_deg, dist);
        if (!is_water(pt_lat, pt_lon)) {
            crossing_lat = pt_lat;
            crossing_lon = pt_lon;
            found = true;
            break;
        }
    }

    // If no coastline is found within max_km, return the input as both origin
    // and coast (degenerate case — caller supplied an open-ocean point far from
    // any coast).
    if (!found)
        return {lat, lon, lat, lon};

    // Origin: 200 km back along the reverse azimuth from the crossing.
    const double back_az = std::fmod(azimuth_deg + 180.0, 360.0);
    auto [origin_lat, origin_lon] =
        geo_destination(crossing_lat, crossing_lon, back_az, offset_km);

    return {origin_lat, origin_lon, crossing_lat, crossing_lon};
}
