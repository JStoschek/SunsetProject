#pragma once
#include <vector>

class OGRPolygon;  // defined in <ogr_geometry.h>; only pointers used here

// Abstract source of land polygon geometry, one 1° tile at a time.
// Implementations parse a specific data format (GSHHG, OSM shapefiles, …)
// and provide OGRPolygon* objects for tiles on demand.
class WaterPolygonSource {
public:
    virtual ~WaterPolygonSource() = default;

    // Append to `out` every land-polygon OGRPolygon* whose bounding box
    // overlaps the 1°×1° tile at (tile_lat, tile_lon).  Polygons are not
    // pre-clipped to the tile extent; the caller must clip before rasterizing.
    // Caller takes ownership of each pointer pushed into `out`.
    virtual void land_polygons_for_tile(int tile_lat, int tile_lon,
                                        std::vector<OGRPolygon*>& out) = 0;
};
