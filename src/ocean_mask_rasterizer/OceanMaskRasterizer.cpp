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

    // Rewind so downstream code can scan from the start.
    file_.seekg(0, std::ios::beg);

    GDALAllRegister();
}

// ---------------------------------------------------------------------------
// Stubs — full implementations in later issues.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Rasterize all GSHHG Level-1 land polygons that overlap the 1°×1° tile
// defined by (tile_lat, tile_lon) into `raster` (kTilePixels×kTilePixels
// bytes, row-major, north-up).  Initial value 1 = water; burned value 0 = land.
// ---------------------------------------------------------------------------
static void rasterize_tile(std::ifstream& file,
                            int tile_lat, int tile_lon,
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

    // --- Collect Level-1 polygons that overlap the tile --------------------
    // Tile geographic extent (in degrees, [-180, 180] longitude convention).
    const double tile_west  = (double)tile_lon;
    const double tile_east  = (double)(tile_lon + 1);
    const double tile_south = (double)tile_lat;
    const double tile_north = (double)(tile_lat + 1);

    std::vector<OGRGeometryH> hgeoms;
    std::vector<OGRGeometry*> owned_geoms;  // for cleanup

    // Clear eofbit (and any other error bits) before rewinding, so that
    // seekg works correctly even if the previous call read to the end of file.
    file.clear();
    file.seekg(0, std::ios::beg);
    while (file) {
        GshhgHeader hdr{};
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!file) break;

        // Byte-swap every field (big-endian on disk).
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

        if (level != 1) {
            // Skip polygon points and continue.
            file.seekg(static_cast<std::streamoff>(sizeof(GshhgPoint)) * n,
                       std::ios::cur);
            continue;
        }

        // Convert bounding box from micro-degrees (0–360 convention) to degrees
        // in [-180, 180].
        auto microdeg_to_lon = [](int32_t v) -> double {
            double d = v * 1e-6;
            return (d > 180.0) ? d - 360.0 : d;
        };
        const double poly_west  = microdeg_to_lon(hdr.west);
        const double poly_east  = microdeg_to_lon(hdr.east);
        const double poly_south = hdr.south * 1e-6;
        const double poly_north = hdr.north * 1e-6;

        // Quick AABB overlap test.
        bool overlaps = (poly_east  >= tile_west)  &&
                        (poly_west  <= tile_east)   &&
                        (poly_north >= tile_south)  &&
                        (poly_south <= tile_north);

        if (!overlaps) {
            file.seekg(static_cast<std::streamoff>(sizeof(GshhgPoint)) * n,
                       std::ios::cur);
            continue;
        }

        // Read polygon points and build an OGRPolygon.
        std::vector<GshhgPoint> pts(n);
        file.read(reinterpret_cast<char*>(pts.data()),
                  static_cast<std::streamsize>(sizeof(GshhgPoint)) * n);
        if (!file) break;

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

        hgeoms.push_back(OGRGeometry::ToHandle(poly));
        owned_geoms.push_back(poly);
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
    rasterize_tile(file_, tile_lat, tile_lon, raster);
    ++rasterize_count_;

    lru_list_.push_front(key);
    auto& entry  = cache_[key];
    entry.bits   = pack_raster(raster);
    entry.lru_it = lru_list_.begin();

    const auto& bits = entry.bits;
    return (bits[idx / 64] >> (idx % 64)) & 1ULL;
}

std::pair<double, double>
OceanMaskRasterizer::ocean_origin_for_ray(double /*azimuth_deg*/,
                                           double lat, double lon)
{
    return {lat, lon};
}
