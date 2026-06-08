// Unit tests for FrozenTileSet<Payload> — the GeoTile-keyed, eviction-free
// frozen tile container shared by FrozenDEM and FrozenOcean.
//
// Behaviors verified:
//   1. find / insert / tile_count key by GeoTile; an unfrozen key -> nullptr.
//   2. owning() resolves to the same entry a from_floor() key inserted.
//   3. Payload buffers are shared (shared_ptr) so adjacent strips reuse them.

#include "FrozenTileSet.h"
#include "GeoTile.h"

#include <cassert>
#include <cstdio>
#include <memory>

static void test_find_insert_count() {
    FrozenTileSet<int> set;
    assert(set.tile_count() == 0);
    assert(set.find(GeoTile::from_floor(37, -123)) == nullptr);

    set.insert(GeoTile::from_floor(37, -123), std::make_shared<const int>(42));
    set.insert(GeoTile::from_floor(38, -122), std::make_shared<const int>(7));
    assert(set.tile_count() == 2);

    const int* a = set.find(GeoTile::from_floor(37, -123));
    assert(a && *a == 42);
    // A coordinate inside the (38,-122) square resolves to the same entry.
    const int* b = set.find(GeoTile::owning(38.5, -121.5));
    assert(b && *b == 7);
    // An unfrozen key reads back as nullptr (the views map this to no-data /
    // open water per their own policy).
    assert(set.find(GeoTile::from_floor(40, -120)) == nullptr);
    std::puts("PASS: find/insert/tile_count keyed by GeoTile; miss -> nullptr");
}

static void test_shared_buffers() {
    // The same buffer can back two sets — cross-strip reuse with no copy.
    auto buf = std::make_shared<const int>(99);
    FrozenTileSet<int> s1, s2;
    s1.insert(GeoTile::from_floor(37, -123), buf);
    s2.insert(GeoTile::from_floor(37, -123), buf);
    assert(s1.find(GeoTile::from_floor(37, -123)) ==
           s2.find(GeoTile::from_floor(37, -123)));
    assert(buf.use_count() == 3);  // buf + the two sets
    std::puts("PASS: payload buffers shared across sets");
}

int main() {
    test_find_insert_count();
    test_shared_buffers();
    std::puts("ALL PASS");
    return 0;
}
