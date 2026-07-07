#pragma once
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
