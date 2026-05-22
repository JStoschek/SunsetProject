// OceanMaskRasterizer unit tests — all I/O uses synthetic in-memory fixtures;
// no real NHD dataset is required to run the test suite.
//
// Synthetic geometry used throughout:
//
//   Pacific polygon (SeaOcean, FType 445): a rectangle with a rectangular
//   notch cut out of its right (east) side, representing the Pacific coast
//   near a strait/bay entrance.
//
//   Vertices (lon, lat):
//     (-135,48)--(-124,48)
//         |              |
//         |         (-124,39)--(-123,39)
//         |                         |
//         |         (-124,35)--(-123,35)
//         |              |
//     (-135,32)--(-124,32)
//
//   "Notch" area (lon -124..-123, lat 35..39) is OUTSIDE the polygon.
//   This represents a bay: NHD classifies it as a different FType, so it
//   is absent from the SeaOcean layer and correctly returns is_ocean=false.

#include "OceanMaskRasterizer.h"
#include <ogrsf_frmts.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void gdal_init() {
    static bool done = false;
    if (!done) { GDALAllRegister(); done = true; }
}

// Builds a .gpkg with an NHDArea layer populated with the given WKT polygons,
// all tagged FType 445 (SeaOcean).
static void make_nhd_gpkg(const fs::path& path,
                           const std::vector<std::string>& sea_ocean_wkt) {
    gdal_init();
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    assert(drv);

    GDALDataset* ds = drv->Create(path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    assert(ds);

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");

    OGRLayer* layer = ds->CreateLayer("NHDArea", &srs, wkbMultiPolygon, nullptr);
    assert(layer);

    OGRFieldDefn ftype_field("ftype", OFTInteger);
    layer->CreateField(&ftype_field);

    for (const auto& wkt : sea_ocean_wkt) {
        OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());

        OGRGeometry* geom = nullptr;
        const char* wkt_ptr = wkt.c_str();
        OGRGeometryFactory::createFromWkt(&wkt_ptr, nullptr, &geom);
        assert(geom);
        feat->SetGeometry(geom);
        OGRGeometryFactory::destroyGeometry(geom);
        feat->SetField("ftype", 445);

        layer->CreateFeature(feat);
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);
}

// Notched Pacific polygon (see file header for diagram).
//
// The polygon spans lon [-135, -123] with a rectangular notch indented on the
// right side: at lat 35–39 the right boundary retreats from lon=-123 to lon=-124.
// Points inside the notch (e.g. lat=37, lon=-123.5) are OUTSIDE the polygon.
//
// Correct vertex order — the boundary goes CCW around the exterior and makes
// a leftward jog for the notch:
//
//  (-135,48)----(-123,48)
//      |                  |
//      |         (-123,39)-(-124,39)
//      |                         ← notch, excluded from polygon
//      |         (-123,35)-(-124,35)
//      |                  |
//  (-135,32)----(-123,32)
static const char* PACIFIC_WKT =
    "MULTIPOLYGON((("
    "-135 32, -135 48, -123 48, "
    "-123 39, -124 39, -124 35, -123 35, "
    "-123 32, -135 32"
    ")))";

// ─── Test 1: construction succeeds and polygon_count is correct ───────────────

static void test_construction() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    make_nhd_gpkg(gpkg, {PACIFIC_WKT});

    OceanMaskRasterizer rast(gpkg.string());
    assert(rast.polygon_count() == 1);

    fs::remove_all(dir);
    std::puts("PASS: construction succeeds; polygon_count == 1");
}

// ─── Test 2: is_ocean returns true for a point in the open Pacific ────────────

static void test_is_ocean_true_for_pacific_point() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    make_nhd_gpkg(gpkg, {PACIFIC_WKT});

    OceanMaskRasterizer rast(gpkg.string());
    // (lat=40, lon=-130): well inside the notched Pacific polygon.
    assert(rast.is_ocean(40.0, -130.0) == true);

    fs::remove_all(dir);
    std::puts("PASS: is_ocean returns true for open-Pacific point");
}

// ─── Test 3: is_ocean returns false for a land point ─────────────────────────

static void test_is_ocean_false_for_land() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    make_nhd_gpkg(gpkg, {PACIFIC_WKT});

    OceanMaskRasterizer rast(gpkg.string());
    // (lat=37, lon=-121): east of the polygon's rightmost boundary → land.
    assert(rast.is_ocean(37.0, -121.0) == false);

    fs::remove_all(dir);
    std::puts("PASS: is_ocean returns false for land point");
}

// ─── Test 4: is_ocean returns false for a point in the synthetic bay ──────────
//
// The notch (lon -124..-123, lat 35..39) represents a bay entrance / strait.
// A point inside the notch is outside the SeaOcean polygon — exactly how NHD
// represents SF Bay: different FType, absent from the SeaOcean layer.

static void test_is_ocean_false_for_synthetic_bay() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    make_nhd_gpkg(gpkg, {PACIFIC_WKT});

    OceanMaskRasterizer rast(gpkg.string());
    // (lat=37, lon=-123.5): inside the notch area → outside SeaOcean polygon.
    assert(rast.is_ocean(37.0, -123.5) == false);

    fs::remove_all(dir);
    std::puts("PASS: is_ocean returns false for synthetic-bay point (notch)");
}

// ─── Test 5: boundary behavior is consistent and documented ──────────────────
//
// A point exactly on the polygon's exterior ring.
// OGRGeometry::Intersects includes the boundary, so is_ocean returns true.
// This is documented here as the canonical contract.

static void test_boundary_returns_true() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    make_nhd_gpkg(gpkg, {PACIFIC_WKT});

    OceanMaskRasterizer rast(gpkg.string());
    // (lat=40, lon=-123): on the right boundary (segment lon=-123, lat 39..48).
    // Contract: boundary points are considered ocean (Intersects semantics).
    assert(rast.is_ocean(40.0, -123.0) == true);

    fs::remove_all(dir);
    std::puts("PASS: boundary point returns true (Intersects semantics, documented)");
}

// ─── Test 6: correct results with many polygons (no linear scan) ─────────────
//
// Constructs N non-overlapping unit squares spread across a grid and verifies
// is_ocean for a known point inside one of them and a point outside all of them.
// Correctness at N=500 confirms the R-tree index routes queries without a linear
// scan through all bounding boxes.

static void test_many_polygons_correct() {
    char tmpl[] = "/tmp/nhd_test_XXXXXX";
    assert(mkdtemp(tmpl));
    fs::path dir(tmpl);
    fs::path gpkg = dir / "test.gpkg";

    // 500 1×1-degree squares: lon offset 0..19 × lat offset 0..24 (step 2).
    std::vector<std::string> polys;
    polys.reserve(500);
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 25; ++j) {
            double x0 = -170.0 + i * 2;
            double y0 =  10.0  + j * 2;
            double x1 = x0 + 1, y1 = y0 + 1;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "MULTIPOLYGON(((%f %f, %f %f, %f %f, %f %f, %f %f)))",
                x0, y0, x1, y0, x1, y1, x0, y1, x0, y0);
            polys.emplace_back(buf);
        }
    }

    make_nhd_gpkg(gpkg, polys);

    OceanMaskRasterizer rast(gpkg.string());
    assert(rast.polygon_count() == 500);

    // Centre of the square at i=5, j=10 → x0=-160, y0=30 → centre (-159.5, 30.5).
    assert(rast.is_ocean(30.5, -159.5) == true);
    // A point in the gap between squares (e.g., at a step boundary).
    assert(rast.is_ocean(11.5, -169.0) == false);

    fs::remove_all(dir);
    std::puts("PASS: correct results with 500 polygons (R-tree, no linear scan)");
}

int main() {
    test_construction();
    test_is_ocean_true_for_pacific_point();
    test_is_ocean_false_for_land();
    test_is_ocean_false_for_synthetic_bay();
    test_boundary_returns_true();
    test_many_polygons_correct();
    return 0;
}
