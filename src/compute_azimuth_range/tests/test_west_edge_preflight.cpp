// Behavioural specification for check_west_edge_offshore (ADR-0015): the
// engine's guard against mis-anchored boxes. Tests drive the function ONLY
// through the WaterQuery seam with analytic fakes — a meridional coast
// (FakeWater) for the all-water / all-land cases, and a latitude-notch fake
// to exercise the tolerated-land-fraction threshold on both sides.
//
// PIPELINE_CONF_PATH is injected by CMake so the tests stay config-driven:
// the threshold under test is the real west_edge_max_land_frac knob.
#include "Fakes.h"
#include "PipelineConfig.h"
#include "WestEdgePreflight.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

// The test box: ~1° of coastal latitude, west edge at kWestLon.
constexpr double kMinLat  = 37.0;
constexpr double kMaxLat  = 38.0;
constexpr double kWestLon = -123.0;
constexpr int    kSamples = 1000;

/// Water everywhere except a land notch over [land_min_lat, land_max_lat) —
/// a headland (or worse) crossing the box's western edge.
struct NotchWater : WaterQuery {
    double land_min_lat, land_max_lat;
    NotchWater(double lo, double hi) : land_min_lat(lo), land_max_lat(hi) {}
    bool is_water(double lat, double) override {
        return lat < land_min_lat || lat >= land_max_lat;
    }
};

/// A notch spanning exactly `land_samples` of the kSamples cell-centre
/// samples, aligned to sample boundaries so the count is exact.
NotchWater notch_covering(int land_samples) {
    const double step = (kMaxLat - kMinLat) / kSamples;
    const int    first = 200;  // arbitrary interior start sample
    return NotchWater(kMinLat + first * step,
                      kMinLat + (first + land_samples) * step);
}

}  // namespace

int main() {
    const PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);
    const double max_frac = config.west_edge_max_land_frac;
    assert(max_frac > 0.0 && max_frac < 1.0 &&
           "west_edge_max_land_frac must be a meaningful fraction");

    // ── Properly anchored: west edge entirely offshore ───────────────────
    {
        FakeWater water(-122.5);  // coast well east of the box's west edge
        const WestEdgeCheck r = check_west_edge_offshore(
            water, kMinLat, kMaxLat, kWestLon, kSamples, max_frac);
        assert(r.offshore && "an offshore west edge must pass");
        assert(r.land_samples == 0);
        assert(r.total_samples == kSamples);
        std::puts("PASS: box with offshore west edge passes");
    }

    // ── Mis-anchored: west edge entirely on land ─────────────────────────
    {
        FakeWater water(-123.5);  // coast west of the box: the box is inland
        const WestEdgeCheck r = check_west_edge_offshore(
            water, kMinLat, kMaxLat, kWestLon, kSamples, max_frac);
        assert(!r.offshore && "an inland west edge must fail");
        assert(r.land_samples == kSamples);
        assert(r.land_fraction == 1.0);
        // The offending range spans (essentially) the whole box.
        assert(r.land_min_lat < kMinLat + 0.01);
        assert(r.land_max_lat > kMaxLat - 0.01);
        std::puts("PASS: fully inland west edge fails");
    }

    // ── Threshold, tolerated side: land fraction exactly at the knob ─────
    {
        const int at_threshold =
            static_cast<int>(std::lround(kSamples * max_frac));
        NotchWater water = notch_covering(at_threshold);
        const WestEdgeCheck r = check_west_edge_offshore(
            water, kMinLat, kMaxLat, kWestLon, kSamples, max_frac);
        assert(r.land_samples == at_threshold);
        assert(r.offshore &&
               "land at exactly the tolerated fraction must still pass");
        std::puts("PASS: stray land at the tolerated fraction passes");
    }

    // ── Threshold, rejected side: one sample over the knob ───────────────
    {
        const int over_threshold =
            static_cast<int>(std::lround(kSamples * max_frac)) + 1;
        NotchWater water = notch_covering(over_threshold);
        const WestEdgeCheck r = check_west_edge_offshore(
            water, kMinLat, kMaxLat, kWestLon, kSamples, max_frac);
        assert(r.land_samples == over_threshold);
        assert(!r.offshore && "land above the tolerated fraction must fail");

        // The reported offending range must name the notch, so the operator
        // knows which way to nudge the box. Samples sit at cell centres, so
        // the reported extremes are within half a step of the notch edges.
        const double step = (kMaxLat - kMinLat) / kSamples;
        assert(r.land_min_lat >= water.land_min_lat &&
               r.land_min_lat <= water.land_min_lat + step);
        assert(r.land_max_lat <= water.land_max_lat &&
               r.land_max_lat >= water.land_max_lat - step);
        std::puts("PASS: over-threshold land fails and names the notch's latitude range");
    }

    std::puts("All west-edge preflight tests passed.");
    return 0;
}
