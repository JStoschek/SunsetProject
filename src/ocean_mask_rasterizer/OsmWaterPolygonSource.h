#pragma once
#include "WaterPolygonSource.h"
#include <memory>
#include <string>
#include <vector>

class GDALDataset;  // forward decl; only owned pointers held here

// Reads OpenStreetMap-derived water geometry and presents it through the
// land-polygon WaterPolygonSource interface.
//
// OSM supplies *water* polygons (the osmdata split `water-polygons` shapefile
// for ocean/coastline, and Geofabrik `natural=water` extracts for inland
// lakes/ponds) — the inverse of GSHHG's land polygons.  This source bridges the
// gap entirely on the read side: for each requested tile it collects the water
// polygons overlapping the tile and returns `tile_bbox − water` as the land
// polygon(s).  The OceanMaskRasterizer and the WaterPolygonSource seam are thus
// unchanged — land still burns over a water-filled raster, lakes appear as holes
// in the land, and open-ocean tiles yield no land at all.
class OsmWaterPolygonSource : public WaterPolygonSource {
public:
    // Opens the OSM water inputs.  `water_polygons_path` is the ocean/coastline
    // split `water-polygons` shapefile; `inland_water_path` is the
    // `natural=water` extract (pass "" to omit inland water).  Throws
    // std::runtime_error if a given path cannot be opened as a vector dataset.
    explicit OsmWaterPolygonSource(const std::string& water_polygons_path,
                                   const std::string& inland_water_path = "");

    ~OsmWaterPolygonSource() override;

    // Non-copyable (owns open GDAL datasets).
    OsmWaterPolygonSource(const OsmWaterPolygonSource&)            = delete;
    OsmWaterPolygonSource& operator=(const OsmWaterPolygonSource&) = delete;

    // Appends the land geometry of the 1°×1° tile at (tile_lat, tile_lon):
    // tile_bbox with every overlapping OSM water body cut out.  Caller takes
    // ownership of each pointer pushed into `out`.
    void land_polygons_for_tile(int tile_lat, int tile_lon,
                                std::vector<OGRPolygon*>& out) override;

private:
    // Opened lazily/owned; one per input file. Null when a path is omitted.
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> water_ds_;
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> inland_ds_;
};
