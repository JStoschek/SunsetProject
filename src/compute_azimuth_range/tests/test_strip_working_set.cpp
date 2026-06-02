// Unit tests for strip_working_set().
//
// Behaviors verified:
//   1. Interior strip (no margin) → only the lat/lon tiles that contain the bbox.
//   2. Strip spanning an integer lat boundary → includes both adjacent lat tiles.
//   3. Tilt margin expands the lat range to pull in an adjacent lat tile.
//   4. Margin smaller than the gap to the nearest boundary → no extra tile added.

#include "StripWorkingSet.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <utility>

using TileKey = std::pair<int,int>;

// ── Cycle 1: interior strip, no tilt ─────────────────────────────────────────
// Strip lat [37.1, 37.8], lon [-122.9, -121.1], margin 0.0
// floor(-122.9) = -123; floor(-121.1) = -122  → two lon tiles
// floor(37.1)   = 37;   floor(37.8)   = 37    → one lat tile
// Expected: {(37,-123), (37,-122)}

static void test_interior_strip_no_margin() {
    auto keys = strip_working_set(37.1, 37.8, -122.9, -121.1, 0.0);

    std::set<TileKey> expected = {{37,-123}, {37,-122}};
    assert(keys == expected);
    std::puts("PASS: interior strip — correct two-lon-tile result");
}

// ── Cycle 2: strip spans integer lat boundary ─────────────────────────────────
// Strip lat [36.7, 37.3], lon [-122.5, -121.5], margin 0.0
// Lat floors: 36 and 37 → two lat rows.
// Lon floors: -123 and -122.
// Expected: {(36,-123), (36,-122), (37,-123), (37,-122)}

static void test_strip_crosses_lat_boundary() {
    auto keys = strip_working_set(36.7, 37.3, -122.5, -121.5, 0.0);

    std::set<TileKey> expected = {
        {36,-123}, {36,-122},
        {37,-123}, {37,-122},
    };
    assert(keys == expected);
    std::puts("PASS: strip-edge — lat boundary tile included");
}

// ── Cycle 3: tilt margin pulls in adjacent lat tile ───────────────────────────
// Strip lat [37.5, 37.7], lon [-122.5, -121.5], margin 0.65
// Expanded lat: [37.5-0.65, 37.7+0.65] = [36.85, 38.35]
// Lat floors: 36, 37, 38 — three lat rows.
// Expected: six tiles across lat 36, 37, 38.

static void test_tilt_margin_expands_lat() {
    auto keys = strip_working_set(37.5, 37.7, -122.5, -121.5, 0.65);

    std::set<TileKey> expected = {
        {36,-123}, {36,-122},
        {37,-123}, {37,-122},
        {38,-123}, {38,-122},
    };
    assert(keys == expected);
    std::puts("PASS: tilt margin — adjacent lat tiles included");
}

// ── Cycle 4: small margin that does not reach the next tile boundary ──────────
// Strip lat [37.1, 37.8], lon [-122.5, -121.5], margin 0.05
// Expanded lat: [37.05, 37.85] — both still floor to 37.
// Expected: same two tiles as the interior case (no extra lat row).

static void test_small_margin_no_extra_tile() {
    auto keys = strip_working_set(37.1, 37.8, -122.5, -121.5, 0.05);

    std::set<TileKey> expected = {{37,-123}, {37,-122}};
    assert(keys == expected);
    std::puts("PASS: small margin — no spurious extra lat tile");
}

int main() {
    test_interior_strip_no_margin();
    test_strip_crosses_lat_boundary();
    test_tilt_margin_expands_lat();
    test_small_margin_no_extra_tile();
    std::puts("ALL PASS");
    return 0;
}
