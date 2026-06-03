// Tests for OceanMaskRasterizer::ocean_origin_for_ray.
// GSHHG_FULL_PATH is injected by CMake at compile time.
#include "OceanMaskRasterizer.h"
#include <cassert>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static constexpr double kPi = M_PI;

// Haversine distance in km between two (lat, lon) pairs (degrees).
static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double R     = 6371.0;
    const double dlat  = (lat2 - lat1) * kPi / 180.0;
    const double dlon  = (lon2 - lon1) * kPi / 180.0;
    const double a     = std::sin(dlat / 2) * std::sin(dlat / 2)
                       + std::cos(lat1 * kPi / 180.0) * std::cos(lat2 * kPi / 180.0)
                       * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * R * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

// Initial bearing in degrees [0, 360) from (lat1,lon1) to (lat2,lon2).
static double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    const double lat1r = lat1 * kPi / 180.0;
    const double lat2r = lat2 * kPi / 180.0;
    const double dlon  = (lon2 - lon1) * kPi / 180.0;
    const double y     = std::sin(dlon) * std::cos(lat2r);
    const double x     = std::cos(lat1r) * std::sin(lat2r)
                       - std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
    return std::fmod(std::atan2(y, x) * 180.0 / kPi + 360.0, 360.0);
}

int main() {
    // -------------------------------------------------------------------------
    // Test parameters: near-shore point off the Marin coast, just west of
    // the outer coastline at Bolinas / Point Reyes area.  Azimuth 90° = due east
    // (toward land).
    // -------------------------------------------------------------------------
    const double az  = 90.0;
    const double lat =  37.9;
    const double lon = -122.7;

    // --- Cycle 5: coast point is on land --------------------------------------
    // The coastline crossing is the first pixel where is_water returns false.
    // The returned coast_lat / coast_lon must therefore fail is_water.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        assert(!omr.is_water(r.coast_lat, r.coast_lon)
               && "coast point must be on land (is_water == false)");
        std::puts("PASS: coast point is on land");
    }

    // --- Cycle 6: coast point lies along the forward azimuth from input -------
    // For azimuth=90° (due east) the bearing from the near-shore input to the
    // coast crossing should be within ±10° of 90°.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        const double b = bearing_deg(lat, lon, r.coast_lat, r.coast_lon);
        // Angular difference, wrapped to [-180, 180].
        double diff = b - az;
        if (diff >  180.0) diff -= 360.0;
        if (diff < -180.0) diff += 360.0;
        assert(std::abs(diff) <= 10.0 && "coast point must lie along forward azimuth (90° ± 10°)");
        std::puts("PASS: coast point lies along forward azimuth (90° ± 10°)");
    }

    // --- Cycle 7: coast point is close to the shoreline -----------------------
    // The near-shore input at 37.9°N, 122.7°W is only a few km west of the
    // Marin coast.  The march stops at the first land pixel, so the crossing
    // must be within 50 km of the input.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        const double dist = haversine_km(lat, lon, r.coast_lat, r.coast_lon);
        assert(dist <= 50.0 && "coast point must be within 50 km of the near-shore input");
        std::puts("PASS: coast point is within 50 km of the near-shore input");
    }

    // =========================================================================
    // Coastal regression guard — Ocean Beach / NW summer-sunset bearing
    // =========================================================================
    //
    // Motivation: commit b350d3e replaced the exact geo_destination() coast-march
    // with an incremental loxodrome approximation.  The crossing moved, shifting
    // along_c in the engine and silently blacking out Ocean Beach at NW sunset
    // azimuths (max_az dropped from 300-301° to −inf/286-291°).  The commit's
    // own tests passed because nothing pinned coastal output.  Cycles 8 and 9
    // close that gap.
    //
    // Geometry:
    //   Sunset azimuth 300° → bearing = (300+180)%360 = 120° (SE, ocean→land).
    //   The engine seeds each ray at the western box edge and calls
    //   ocean_origin_for_ray(120°, seed_lat, min_lon).  For the Ocean Beach cell
    //   at (37.745°N, 122.508°W) the seed_lat is ≈37.760°N.
    //   The exact great-circle march finds the coast at ≈(37.540°N, 122.518°W),
    //   placing Ocean Beach ≈270 m inland of the crossing (dist > 0, visible).
    //   Any march change that shifts crossing east of lon −122.508 makes
    //   along_c > along_ob, giving dist < 0 and silently hiding Ocean Beach.

    // --- Cycle 8: NW sunset crossing lands in the Ocean Beach longitude band ---
    // From lat=37.760°N, lon=123.0°W in the open Pacific, marching SE at bearing
    // 120° (the NW-summer-sunset ocean→land bearing) with production step size
    // (0.05 km ≈ 5 DEM cells), the coast crossing must lie within the
    // Ocean Beach / Outer Sunset longitude band: [−122.540°, −122.505°].
    // The lower bound keeps the test from accepting a crossing at a grossly wrong
    // feature; the upper bound is the critical guard — it asserts the crossing is
    // west of the Ocean Beach shoreline at ~−122.508°, so that the beach cells
    // have positive along-ray distance (dist > 0) and are therefore visible.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        // Production parameters: step=0.05 km, max=100 km.
        auto r = omr.ocean_origin_for_ray(120.0, 37.760, -123.0, 0.05, 100.0);

        assert(!omr.is_water(r.coast_lat, r.coast_lon) &&
               "NW-sunset crossing must be on land");

        // Longitude band: crossing must be west of Ocean Beach (~−122.508°).
        // A march error that pushes the crossing east of −122.505° would place
        // Ocean Beach seaward (dist < 0) and silence it at sunset az ≈ 300°.
        assert(r.coast_lon <= -122.505 &&
               "NW-sunset crossing must be west of Ocean Beach (lon ≤ −122.505°)");
        assert(r.coast_lon >= -122.540 &&
               "NW-sunset crossing must not overshoot west of the longitude band");

        // Latitude band: SE march from 37.760° reaches the coast south of OB.
        assert(r.coast_lat >= 37.52 && r.coast_lat <= 37.56 &&
               "NW-sunset crossing latitude must fall in the Ocean Beach strip");

        std::puts("PASS: NW-sunset crossing lands in the Ocean Beach longitude band");
    }

    // --- Cycle 9: production-seed crossing is pinned west of the OB shoreline --
    // The engine calls ocean_origin_for_ray from the seed latitude for the Ocean
    // Beach ray (≈37.970°N, derived from the rotated-frame geometry).  The exact
    // great-circle march finds the crossing at ≈(37.747°N, −122.511°W) — well
    // west of the Ocean Beach shoreline at −122.508°.
    // This cycle pins that crossing to within ±0.003° in longitude (~265 m),
    // tight enough that any march change shifting the crossing east by more than
    // one DEM tile width would be caught.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(120.0, 37.970, -123.0, 0.05, 100.0);

        assert(!omr.is_water(r.coast_lat, r.coast_lon) &&
               "engine-seed NW-sunset crossing must be on land");

        // Critical lower bound: crossing must remain west of Ocean Beach so that
        // the beach cells (lon ≈ −122.508°) are inland (dist > 0) and visible.
        assert(r.coast_lon <= -122.508 &&
               "engine-seed crossing must be strictly west of Ocean Beach");

        // Tight upper bound: crossing must not move unreasonably far west.
        assert(r.coast_lon >= -122.515 &&
               "engine-seed crossing must not be more than 0.007° west of expected");

        // Latitude pin: the SE march from ≈37.970° reaches the coast near 37.747°.
        assert(r.coast_lat >= 37.740 && r.coast_lat <= 37.756 &&
               "engine-seed crossing latitude must be near 37.747°N");

        std::puts("PASS: engine-seed NW-sunset crossing is pinned west of Ocean Beach");
    }

    // --- Cycle 10: crossing is invariant to seed distance (box-invariance) -----
    // The engine seeds every ray at the bounding box's western edge, so the seed
    // moves offshore as the box widens (the production extent seeds ~57 km out,
    // a tight diagnostic box ~4 km out).  A spherical great-circle march curves
    // away from the engine's flat ray by ~d²/2R, an error that grows with the
    // seed→coast distance: between a near and a far edge the Ocean Beach crossing
    // drifted ~234 m, pushing it east of the shoreline and silently turning the
    // beach "not visible" at NW sunset azimuths (~300°).  The flat-earth rhumb
    // march makes the crossing depend only on the ray and the coastline, so the
    // same ray reaches the same crossing regardless of how far back it is seeded.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);

        // Far seed in the open Pacific, marching SE (bearing 120°) to Ocean Beach.
        const auto far_c = omr.ocean_origin_for_ray(120.0, 37.970, -123.0, 0.05, 100.0);

        // Build a NEAR seed only ~3 km offshore on the SAME ray, by stepping back
        // (reverse bearing) from the far crossing in the flat metric.
        const double km_per_deg = 6371.0 * kPi / 180.0;
        const double th         = 120.0 * kPi / 180.0;
        const double back_km    = 3.0;
        const double near_lat = far_c.coast_lat - back_km * std::cos(th) / km_per_deg;
        const double near_lon = far_c.coast_lon
                              - back_km * std::sin(th)
                                / (km_per_deg * std::cos(near_lat * kPi / 180.0));
        const auto near_c = omr.ocean_origin_for_ray(120.0, near_lat, near_lon, 0.05, 100.0);

        // A near seed (~3 km baseline) and a far seed (~57 km baseline) on the
        // same ray must reach the same crossing.  Tolerance 0.0003° (~33 m) is
        // far below the ~234 m great-circle drift this guards against, yet well
        // above the flat march's sub-metre residual.
        assert(std::abs(near_c.coast_lat - far_c.coast_lat) < 0.0003 &&
               "coast crossing latitude must be invariant to seed distance");
        assert(std::abs(near_c.coast_lon - far_c.coast_lon) < 0.0003 &&
               "coast crossing longitude must be invariant to seed distance");

        std::puts("PASS: coast crossing is invariant to seed distance (box-invariance)");
    }

    std::puts("ALL PASS");
    return 0;
}
