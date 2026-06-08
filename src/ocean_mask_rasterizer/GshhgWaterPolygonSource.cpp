#include "GshhgWaterPolygonSource.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <ogr_geometry.h>

// GSHHG binary files are big-endian.
static int32_t be32(int32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    std::memcpy(&v, &u, 4);
#endif
    return v;
}

static double microdeg_to_lon(int32_t v) {
    double d = v * 1e-6;
    return (d > 180.0) ? d - 360.0 : d;
}

GshhgWaterPolygonSource::GshhgWaterPolygonSource(const std::string& path)
    : path_(path)
{
    file_.open(path, std::ios::binary);
    if (!file_.is_open())
        throw std::runtime_error(
            "GshhgWaterPolygonSource: cannot open GSHHG file: " + path);

    // Validate by reading the first polygon header.
    GshhgHeader hdr{};
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!file_)
        throw std::runtime_error(
            "GshhgWaterPolygonSource: file too short to contain a GSHHG header: "
            + path);

    hdr.id   = be32(hdr.id);
    hdr.n    = be32(hdr.n);
    hdr.flag = be32(hdr.flag);

    int level = hdr.flag & 0xFF;
    if (hdr.id != 0 || hdr.n < 1 || level != 1)
        throw std::runtime_error(
            "GshhgWaterPolygonSource: file does not look like a GSHHG binary: "
            + path);

    // Rewind for the index build pass.
    file_.seekg(0, std::ios::beg);

    // Build spatial index: scan every polygon header (skip point data) and
    // record a PolyRef for each Level-1 polygon in every 1°×1° tile its
    // bounding box overlaps.
    while (file_) {
        const std::streamoff hdr_offset = file_.tellg();

        GshhgHeader h{};
        file_.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!file_) break;

        h.id        = be32(h.id);
        h.n         = be32(h.n);
        h.flag      = be32(h.flag);
        h.west      = be32(h.west);
        h.east      = be32(h.east);
        h.south     = be32(h.south);
        h.north     = be32(h.north);
        h.area      = be32(h.area);
        h.area_full = be32(h.area_full);
        h.container = be32(h.container);
        h.ancestor  = be32(h.ancestor);

        const int level_h = h.flag & 0xFF;
        const int n       = h.n;

        file_.seekg(static_cast<std::streamoff>(sizeof(GshhgPoint)) * n,
                    std::ios::cur);

        if (level_h != 1) continue;

        const double poly_west  = microdeg_to_lon(h.west);
        const double poly_east  = microdeg_to_lon(h.east);
        const double poly_south = h.south * 1e-6;
        const double poly_north = h.north * 1e-6;

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

    file_.clear();
}

void GshhgWaterPolygonSource::land_polygons_for_tile(
    int tile_lat, int tile_lon, std::vector<OGRPolygon*>& out)
{
    const GeoTile key{tile_lat, tile_lon};
    auto idx_it = index_.find(key);
    if (idx_it == index_.end()) return;

    for (const PolyRef& ref : idx_it->second) {
        file_.clear();
        file_.seekg(ref.offset, std::ios::beg);

        GshhgHeader hdr{};
        file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!file_) continue;

        const int n = ref.n;

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
        ring->setPoint(n, ring->getX(0), ring->getY(0));

        OGRPolygon* poly = new OGRPolygon();
        poly->addRingDirectly(ring);
        out.push_back(poly);
    }
}
