#pragma once
#include "GeoTile.h"
#include "WaterPolygonSource.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// GSHHG binary header (big-endian on disk).
// See data/GSHHG/data/0-data/README.TXT for the full format spec.
struct GshhgHeader {
    int32_t id;
    int32_t n;
    int32_t flag;  // level | version<<8 | greenwich<<16 | source<<24 | river<<25
    int32_t west;  // extent in micro-degrees
    int32_t east;
    int32_t south;
    int32_t north;
    int32_t area;       // 1/10 km²
    int32_t area_full;
    int32_t container;
    int32_t ancestor;
};

struct GshhgPoint {
    int32_t x;  // longitude in micro-degrees
    int32_t y;  // latitude in micro-degrees
};

// A compact reference to a GSHHG polygon: its byte offset in the file and the
// number of points (so the caller can seek past its point data without
// re-reading the header).
struct PolyRef {
    std::streamoff offset;
    int            n;
};

// Reads land polygon geometry from a GSHHG binary file (e.g. gshhs_f.b).
// Builds a spatial index at construction; subsequent land_polygons_for_tile
// calls seek only to the relevant polygon records.
class GshhgWaterPolygonSource : public WaterPolygonSource {
public:
    // Opens the GSHHG binary at `path`, validates the first header, and builds
    // the spatial index.  Throws std::runtime_error on failure.
    explicit GshhgWaterPolygonSource(const std::string& path);

    // Non-copyable (owns an open file stream).
    GshhgWaterPolygonSource(const GshhgWaterPolygonSource&)            = delete;
    GshhgWaterPolygonSource& operator=(const GshhgWaterPolygonSource&) = delete;

    // Appends to `out` one OGRPolygon* per Level-1 land polygon whose bounding
    // box overlaps the given 1°×1° tile.  Polygons are not clipped to the tile
    // extent.  Caller takes ownership of every pointer pushed into `out`.
    void land_polygons_for_tile(int tile_lat, int tile_lon,
                                std::vector<OGRPolygon*>& out) override;

private:
    // Internal spatial index keyed by the tile a polygon's bbox overlaps —
    // signed SW-corner GeoTile, the same labeling land_polygons_for_tile uses.
    std::ifstream file_;
    std::string   path_;
    std::unordered_map<GeoTile, std::vector<PolyRef>, GeoTileHash> index_;
};
