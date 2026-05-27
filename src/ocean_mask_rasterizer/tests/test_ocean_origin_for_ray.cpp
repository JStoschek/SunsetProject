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

    // --- Cycle 1: origin is in the open Pacific --------------------------------
    // For a near-shore input west of the Marin coast, marching east hits the
    // coast; the returned origin must be well into the open Pacific (lon < -123.5°).
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        assert(r.origin_lon < -123.5 && "origin must be in open Pacific, not near shore");
        std::puts("PASS: origin is in open Pacific (lon < -123.5°W)");
    }

    // --- Cycle 2: origin is at least 190 km west of the near-shore input ------
    // 200 km back from the crossing + crossing-to-input distance (a few km) ≥ 190 km.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        const double dist = haversine_km(lat, lon, r.origin_lat, r.origin_lon);
        assert(dist >= 190.0 && "origin must be >= 190 km west of near-shore input");
        std::puts("PASS: origin is >= 190 km from near-shore input");
    }

    // --- Cycle 3: origin passes is_water --------------------------------------
    // The 200 km offset into open Pacific should land on water.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        assert(omr.is_water(r.origin_lat, r.origin_lon) && "origin must be in water");
        std::puts("PASS: origin passes is_water");
    }

    // --- Cycle 4: origin lies along back-azimuth from input -------------------
    // For azimuth=90° the back-azimuth is 270° (due west).  The bearing from
    // the near-shore input to the origin should be within ±10° of 270°.
    {
        OceanMaskRasterizer omr(GSHHG_FULL_PATH);
        auto r = omr.ocean_origin_for_ray(az, lat, lon);
        const double b = bearing_deg(lat, lon, r.origin_lat, r.origin_lon);
        // Angular difference, wrapped to [-180, 180].
        double diff = b - 270.0;
        if (diff >  180.0) diff -= 360.0;
        if (diff < -180.0) diff += 360.0;
        assert(std::abs(diff) <= 10.0 && "origin must lie along back-azimuth (270° ± 10°)");
        std::puts("PASS: origin lies along back-azimuth (270° ± 10°)");
    }

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

    std::puts("ALL PASS");
    return 0;
}
