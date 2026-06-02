// Fixture-driven tests for OsmWaterPolygonSource: known OSM-style water polygons
// (ocean/coastline split water-polygons + natural=water inland) rasterize to the
// expected is_water bits through the unchanged OceanMaskRasterizer.
//
// Modeled on test_is_water_rasterization.cpp, but the fixtures are tiny
// shapefiles written at runtime with OGR rather than the real gshhs_f.b — so the
// "known polygons → expected water bits" mapping lives in the test itself.
#include "OceanMaskRasterizer.h"
#include "OsmWaterPolygonSource.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogr_geometry.h>
#include <ogrsf_frmts.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// One closed rectangular ring [lon0,lon1] × [lat0,lat1].
static OGRPolygon rect(double lon0, double lat0, double lon1, double lat1) {
    OGRLinearRing ring;
    ring.addPoint(lon0, lat0);
    ring.addPoint(lon1, lat0);
    ring.addPoint(lon1, lat1);
    ring.addPoint(lon0, lat1);
    ring.addPoint(lon0, lat0);
    OGRPolygon poly;
    poly.addRing(&ring);
    return poly;
}

// Write `polys` as polygon features into an ESRI Shapefile at `path`.
static void write_shapefile(const std::string& path,
                            const std::vector<OGRPolygon>& polys) {
    GDALDriver* drv =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    assert(drv && "ESRI Shapefile driver must be available");

    // Overwrite any pre-existing fixture.
    if (fs::exists(path)) drv->Delete(path.c_str());

    GDALDataset* ds =
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    assert(ds && "failed to create fixture shapefile");

    OGRLayer* layer =
        ds->CreateLayer("water", nullptr, wkbPolygon, nullptr);
    assert(layer && "failed to create layer");

    for (const OGRPolygon& poly : polys) {
        OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feat->SetGeometry(&poly);
        (void)layer->CreateFeature(feat);
        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
}

int main() {
    GDALAllRegister();

    fs::path dir = fs::temp_directory_path() / "osm_water_src_test";
    fs::create_directories(dir);

    // --- Cycle 1 (tracer bullet): a coastline splits water from land ---------
    // A water polygon covers the west half of tile (0,0); the east half has no
    // water.  This proves the whole path at once: the shapefile is read, the
    // ocean side rasterizes as water, and — crucially — inverting the water
    // polygon against the tile makes the land side (where OSM gives no water)
    // read as land, the inverse of GSHHG's land-polygon convention.
    {
        const std::string water = (dir / "coast_west.shp").string();
        write_shapefile(water, { rect(-0.1, -0.1, 0.5, 1.1) });

        OceanMaskRasterizer omr(
            std::make_unique<OsmWaterPolygonSource>(water, ""));
        assert(omr.is_water(0.5, 0.25)  && "west half (ocean) must be water");
        assert(!omr.is_water(0.5, 0.75) && "east half (no water) must be land");
        std::puts("PASS: coastline splits water (west) from land (east)");
    }

    // --- Cycle 2: the coastline lands within one pixel -----------------------
    // Same west-half ocean fixture (boundary at lon = 0.5).  Marching east one
    // pixel at a time, the water→land transition must occur within a single
    // 1/3-arc-second pixel of the fixture's coastline.
    {
        constexpr double kPixel = 1.0 / 10800.0;  // 1/3 arc-second in degrees
        const std::string water = (dir / "coast_west.shp").string();
        write_shapefile(water, { rect(-0.1, -0.1, 0.5, 1.1) });

        OceanMaskRasterizer omr(
            std::make_unique<OsmWaterPolygonSource>(water, ""));

        // Walk east across the boundary; find the first land pixel.
        const double lat = 0.5;
        double transition = -1.0;
        for (double lon = 0.45; lon <= 0.55; lon += kPixel) {
            if (!omr.is_water(lat, lon)) { transition = lon; break; }
        }
        assert(transition > 0.0 && "must find a water→land transition");
        assert(std::abs(transition - 0.5) <= kPixel &&
               "coastline must resolve within one pixel of lon=0.5");
        std::puts("PASS: coastline resolves within one pixel");
    }

    // --- Cycle 3: an inland natural=water lake reads as water ----------------
    // The ocean source has no water in this tile (all land); the separate
    // natural=water source carries one small lake polygon.  is_water must be
    // true inside the lake and false on the surrounding land — the lake is a
    // hole cut into the inverted land polygon.
    {
        const std::string ocean  = (dir / "no_ocean.shp").string();
        const std::string inland = (dir / "lake.shp").string();
        write_shapefile(ocean,  {});                              // no ocean here
        write_shapefile(inland, { rect(0.40, 0.40, 0.60, 0.60) }); // lake

        OceanMaskRasterizer omr(
            std::make_unique<OsmWaterPolygonSource>(ocean, inland));
        assert(omr.is_water(0.50, 0.50)  && "inside the lake must be water");
        assert(!omr.is_water(0.10, 0.10) && "land away from the lake must be land");
        assert(!omr.is_water(0.90, 0.90) && "land away from the lake must be land");
        std::puts("PASS: inland natural=water lake is water");
    }

    // --- Cycle 4: the coast crossing lands on the boundary within one pixel --
    // Drive the rasterizer's coast march (ocean_origin_for_ray) over the OSM
    // source.  Starting in the ocean west of the fixture coastline and marching
    // due east, the returned crossing must land on the OSM water→land boundary
    // (lon = 0.5) within one pixel.  A sub-pixel march step is used so the
    // crossing is limited by the mask resolution, not the step size.
    {
        constexpr double kPixel = 1.0 / 10800.0;  // 1/3 arc-second in degrees
        const std::string water = (dir / "coast_west.shp").string();
        write_shapefile(water, { rect(-0.1, -0.1, 0.5, 1.1) });

        OceanMaskRasterizer omr(
            std::make_unique<OsmWaterPolygonSource>(water, ""));

        // Start at (0.5, 0.45) — open water — and march east (azimuth 90°).
        auto r = omr.ocean_origin_for_ray(/*az=*/90.0, /*lat=*/0.5, /*lon=*/0.45,
                                          /*step_km=*/0.005, /*max_km=*/10.0);
        assert(std::abs(r.coast_lon - 0.5) <= kPixel &&
               "coast crossing must land within one pixel of lon=0.5");
        assert(!omr.is_water(r.coast_lat, r.coast_lon) &&
               "the crossing point itself must be land");
        std::puts("PASS: coast crossing lands within one pixel of the boundary");
    }

    std::puts("ALL PASS");
    return 0;
}
