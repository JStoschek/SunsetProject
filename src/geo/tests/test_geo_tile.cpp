// Unit tests for GeoTile — the typed 1°×1° tile identity (CONTEXT.md).
//
// Behaviors verified:
//   1. owning() floors lat/lon to the signed SW corner, including negative lon.
//   2. usgs_nw() projects to the USGS NW-corner (n, |w|) naming convention.
//   3. from_usgs() / from_floor() round-trip with the projections.
//   4. owning() == from_usgs() for the same square (the convention bridge that
//      replaces DEMTileLoader::freeze's old +0.5 hack).
//   5. Equality / ordering / hash distinguish adjacent tiles.

#include "GeoTile.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <utility>

// Compile-time checks: GeoTile is a constexpr value usable in constant context.
static_assert(GeoTile::owning(37.5, -122.5) == GeoTile::from_floor(37, -123),
              "owning floors to signed SW corner");
static_assert(GeoTile::owning(37.5, -122.5).usgs_nw() == std::pair<int,int>{38, 123},
              "usgs_nw projects to USGS NW corner");
static_assert(GeoTile::from_usgs(38, 123) == GeoTile::owning(37.5, -122.5),
              "from_usgs round-trips to the same square");

// ── Cycle 1: owning() floors to the signed SW corner ─────────────────────────
static void test_owning_floors_signed() {
    // Interior point of the n38w123 square (lat [37,38], lon [-123,-122]).
    const GeoTile t = GeoTile::owning(37.5, -122.5);
    assert(t.south == 37 && t.west == -123);
    assert((t.floor() == std::pair<int,int>{37, -123}));

    // Point just east of the -122 meridian lands in the next lon tile.
    const GeoTile e = GeoTile::owning(37.5, -121.0001);
    assert(e.south == 37 && e.west == -122);
    std::puts("PASS: owning() floors lat/lon to the signed SW corner");
}

// ── Cycle 2: usgs_nw() projects to USGS NW-corner naming ─────────────────────
static void test_usgs_projection() {
    assert((GeoTile::owning(37.5, -122.5).usgs_nw() == std::pair<int,int>{38, 123}));
    assert((GeoTile::owning(38.9, -123.1).usgs_nw() == std::pair<int,int>{39, 124}));
    // On an integer boundary: lon exactly -123 belongs to the w123 column.
    assert((GeoTile::owning(37.0, -123.0).usgs_nw() == std::pair<int,int>{38, 123}));
    std::puts("PASS: usgs_nw() projects to the USGS NW-corner (n, |w|)");
}

// ── Cycle 3: from_usgs() / from_floor() round-trips ──────────────────────────
static void test_round_trips() {
    for (int n = 37; n <= 40; ++n) {
        for (int w = 121; w <= 125; ++w) {
            const GeoTile t = GeoTile::from_usgs(n, w);
            assert((t.usgs_nw() == std::pair<int,int>{n, w}));
            assert(GeoTile::from_floor(t.south, t.west) == t);
        }
    }
    std::puts("PASS: from_usgs / from_floor round-trip through the projections");
}

// ── Cycle 4: the convention bridge — owning == from_usgs for one square ───────
static void test_owning_matches_usgs_bridge() {
    // The old DEMTileLoader::freeze bridged a geographic-floor key to the USGS
    // key by sampling the cell interior: dem_tile_key(geo_lat+0.5, geo_lon+0.5).
    // GeoTile makes the two labelings the same identity with no magic offset.
    const GeoTile floor_key = GeoTile::from_floor(37, -123);
    const auto [n, w] = floor_key.usgs_nw();
    assert(GeoTile::from_usgs(n, w) == floor_key);
    assert(GeoTile::owning(37 + 0.5, -123 + 0.5) == floor_key);
    std::puts("PASS: owning() == from_usgs() — no +0.5 bridge needed");
}

// ── Cycle 5: equality, ordering, hash distinguish adjacent tiles ─────────────
static void test_equality_order_hash() {
    const GeoTile a = GeoTile::from_floor(37, -123);
    const GeoTile b = GeoTile::from_floor(37, -122);  // east neighbor
    const GeoTile c = GeoTile::from_floor(38, -123);  // north neighbor
    assert(a != b && a != c && b != c);

    // Usable as a std::set key (the Strip Working Set relies on this).
    std::set<GeoTile> s = {a, b, c, a};
    assert(s.size() == 3);

    GeoTileHash h;
    assert(h(a) != h(b));
    assert(h(a) != h(c));
    assert(h(a) == h(GeoTile::from_floor(37, -123)));
    std::puts("PASS: equality / ordering / hash distinguish adjacent tiles");
}

int main() {
    test_owning_floors_signed();
    test_usgs_projection();
    test_round_trips();
    test_owning_matches_usgs_bridge();
    test_equality_order_hash();
    std::puts("ALL PASS");
    return 0;
}
