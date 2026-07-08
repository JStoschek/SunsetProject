#pragma once
#include <algorithm>
#include <cmath>

#include "HorizonSweepEngine.h"  // WaterQuery seam (ADR-0009)

// West-edge preflight (ADR-0015). Every ray seeds at the box's western edge
// and marches east to the coast (ADR-0008), so a box whose west edge is not
// offshore produces physically-garbage output that the encoder cannot
// distinguish from legitimately-all-blocked land. This check samples
// is_water along the west-edge column before any sweep runs, so a
// mis-anchored box fails loudly with no tif written. A small land fraction
// is tolerated (a stray rock or clipped headland is not mis-anchoring).

struct WestEdgeCheck {
    bool   offshore      = true;  ///< land fraction within tolerance
    int    land_samples  = 0;
    int    total_samples = 0;
    double land_fraction = 0.0;
    double land_min_lat  = 0.0;   ///< offending latitude range;
    double land_max_lat  = 0.0;   ///< valid iff land_samples > 0
};

/// Sample is_water at `sample_count` evenly spaced points (cell centres) on
/// the west-edge column [min_lat, max_lat] × west_lon. The box passes while
/// the land fraction is <= max_land_frac.
inline WestEdgeCheck check_west_edge_offshore(WaterQuery& water,
                                              double min_lat, double max_lat,
                                              double west_lon,
                                              int sample_count,
                                              double max_land_frac) {
    WestEdgeCheck r;
    r.total_samples = sample_count;
    if (sample_count <= 0) return r;

    const double step = (max_lat - min_lat) / sample_count;
    for (int i = 0; i < sample_count; ++i) {
        const double lat = min_lat + (i + 0.5) * step;
        if (!water.is_water(lat, west_lon)) {
            if (r.land_samples == 0) r.land_min_lat = lat;
            r.land_max_lat = lat;  // samples ascend, so the last land is max
            ++r.land_samples;
        }
    }
    r.land_fraction = static_cast<double>(r.land_samples) / r.total_samples;
    r.offshore = r.land_fraction <= max_land_frac;
    return r;
}

// Coast-march coverage preflight. The per-ray coast search gives up after
// coast_march_max_km; a ray whose coastline crossing lies beyond that yields
// NO samples at all, so its pixels stay black at every azimuth — a silent
// blackout indistinguishable from legitimately-blocked land (observed at
// Moss Landing: the back of Monterey Bay sits ~150 km along-ray from the
// box's west edge with a 100 km give-up). The give-up must cover the
// farthest in-box pixel from its west-edge seed: the box's longitude width
// divided by sin(bearing) at the steepest swept azimuth.

struct CoastMarchCheck {
    bool   covers      = true;
    double required_km = 0.0;  ///< worst-case along-ray span of the box
};

/// Closed-form check that `coast_march_max_km` covers the whole box across
/// the swept azimuth window. Bearing = azimuth + 180; sin is concave over
/// (0°, 180°), so the minimum over the window sits at one of its endpoints.
/// Width uses cos(min_lat) — the box's flat-frame anchor and its widest
/// parallel in the northern hemisphere — matching the engine's own metric.
inline CoastMarchCheck check_coast_march_covers_box(
        double min_lat, double min_lon, double max_lon,
        double meters_per_degree_lat,
        double azimuth_min_deg, double azimuth_max_deg,
        double coast_march_max_km) {
    constexpr double kPi = 3.14159265358979323846;
    const double width_m = (max_lon - min_lon) * meters_per_degree_lat *
                           std::cos(min_lat * kPi / 180.0);
    const double s1 = std::fabs(std::sin((azimuth_min_deg + 180.0) * kPi / 180.0));
    const double s2 = std::fabs(std::sin((azimuth_max_deg + 180.0) * kPi / 180.0));
    const double min_sin = std::max(std::min(s1, s2), 1e-9);

    CoastMarchCheck r;
    r.required_km = width_m / min_sin / 1000.0;
    r.covers      = coast_march_max_km >= r.required_km;
    return r;
}
