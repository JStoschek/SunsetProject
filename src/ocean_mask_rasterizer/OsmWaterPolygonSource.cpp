#include "OsmWaterPolygonSource.h"

#include <stdexcept>

#include <gdal_priv.h>
#include <ogr_geometry.h>
#include <ogrsf_frmts.h>

// ---------------------------------------------------------------------------
static void close_dataset(GDALDataset* ds) {
    if (ds) GDALClose(ds);
}

static std::unique_ptr<GDALDataset, void (*)(GDALDataset*)>
open_vector(const std::string& path) {
    if (path.empty())
        return {nullptr, close_dataset};
    GDALAllRegister();  // idempotent; must precede any open (ctor init list)
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds)
        throw std::runtime_error(
            "OsmWaterPolygonSource: cannot open vector dataset: " + path);
    return {ds, close_dataset};
}

OsmWaterPolygonSource::OsmWaterPolygonSource(
    const std::string& water_polygons_path,
    const std::string& inland_water_path)
    : water_ds_(open_vector(water_polygons_path)),
      inland_ds_(open_vector(inland_water_path))
{}

OsmWaterPolygonSource::~OsmWaterPolygonSource() = default;

// Append `geom` to `out` as one or more owned OGRPolygon*, flattening a
// MultiPolygon into its constituent polygons and ignoring non-areal results.
static void emit_polygons(const OGRGeometry* geom,
                          std::vector<OGRPolygon*>& out) {
    if (!geom || geom->IsEmpty()) return;
    const OGRwkbGeometryType type = wkbFlatten(geom->getGeometryType());
    if (type == wkbPolygon) {
        out.push_back(geom->toPolygon()->clone());
    } else if (type == wkbMultiPolygon) {
        const OGRMultiPolygon* mp = geom->toMultiPolygon();
        for (const OGRPolygon* sub : mp)
            if (sub && !sub->IsEmpty())
                out.push_back(sub->clone());
    }
}

// Accumulate every water polygon in `ds` overlapping the tile bbox into `water`.
static void collect_water(GDALDataset* ds, const OGREnvelope& env,
                          OGRMultiPolygon& water) {
    if (!ds) return;
    for (OGRLayer* layer : ds->GetLayers()) {
        layer->SetSpatialFilterRect(env.MinX, env.MinY, env.MaxX, env.MaxY);
        layer->ResetReading();
        for (auto& feat : *layer) {
            const OGRGeometry* g = feat->GetGeometryRef();
            if (!g || g->IsEmpty()) continue;
            const OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
            if (t == wkbPolygon) {
                water.addGeometry(g);
            } else if (t == wkbMultiPolygon) {
                for (const OGRPolygon* sub : g->toMultiPolygon())
                    if (sub && !sub->IsEmpty())
                        water.addGeometry(sub);
            }
        }
        layer->SetSpatialFilter(nullptr);
    }
}

void OsmWaterPolygonSource::land_polygons_for_tile(
    int tile_lat, int tile_lon, std::vector<OGRPolygon*>& out)
{
    // Tile bounding box [tile_lon, tile_lon+1] × [tile_lat, tile_lat+1].
    OGRLinearRing bbox_ring;
    bbox_ring.addPoint(tile_lon,     tile_lat);
    bbox_ring.addPoint(tile_lon + 1, tile_lat);
    bbox_ring.addPoint(tile_lon + 1, tile_lat + 1);
    bbox_ring.addPoint(tile_lon,     tile_lat + 1);
    bbox_ring.addPoint(tile_lon,     tile_lat);
    OGRPolygon tile_bbox;
    tile_bbox.addRing(&bbox_ring);

    OGREnvelope env;
    tile_bbox.getEnvelope(&env);

    // Collect all OSM water (ocean/coastline + inland) overlapping the tile.
    OGRMultiPolygon water;
    collect_water(water_ds_.get(),  env, water);
    collect_water(inland_ds_.get(), env, water);

    // No water here → the whole tile is land.
    if (water.IsEmpty()) {
        out.push_back(tile_bbox.clone());
        return;
    }

    // The split `water-polygons` shapefile (and OSM `natural=water` extracts)
    // contain many adjacent/overlapping polygons that share edges; feeding that
    // raw collection straight into an overlay op yields a non-noded-intersection
    // TopologyException and a null result.  Dissolve the collection into one
    // valid, noded geometry first.  Fall back to the raw collection if the
    // union itself fails (better a topology warning than dropping the tile).
    std::unique_ptr<OGRGeometry> dissolved(water.UnaryUnion());
    const OGRGeometry* cut = dissolved ? dissolved.get()
                                       : static_cast<const OGRGeometry*>(&water);

    // Land is the tile with every water body cut out (lakes become holes).
    std::unique_ptr<OGRGeometry> land(tile_bbox.Difference(cut));
    emit_polygons(land.get(), out);
}
