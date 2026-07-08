// Behavioural specification for the run preflights: check_west_edge_offshore
// (ADR-0015, the guard against mis-anchored boxes) and
// check_coast_march_covers_box (the guard against a coast-march give-up
// shorter than the box — the silent Moss Landing blackout). West-edge tests
// drive the function ONLY through the WaterQuery seam with analytic fakes —
// a meridional coast (FakeWater) for the all-water / all-land cases, and a
// latitude-notch fake to exercise the tolerated-land-fraction threshold on
// both sides. The coverage check is pure closed-form geometry.
//
// PIPELINE_CONF_PATH is injected by CMake so the tests stay config-driven:
// the thresholds under test are the real config knobs.
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

    // ── Coast-march coverage: the production box needs > 100 km ──────────
    // The Moss Landing regression, closed form: the production box spans
    // 1.722° of longitude (~155 km at its south edge), and at the steepest
    // swept azimuth the along-ray span approaches 200 km. A 100 km give-up
    // must FAIL this box (it silently blacked out the back of Monterey Bay);
    // the check's own required_km must pass it.
    {
        const double b_min_lat = 36.1442, b_min_lon = -123.2651,
                     b_max_lon = -121.5431;
        const CoastMarchCheck fail = check_coast_march_covers_box(
            b_min_lat, b_min_lon, b_max_lon, config.meters_per_degree_lat,
            config.azimuth_min_deg, config.azimuth_max_deg, 100.0);
        assert(!fail.covers &&
               "a 100 km give-up must fail the 1.7-degree production box");
        assert(fail.required_km > 150.0 && fail.required_km < 250.0 &&
               "required span must be the ~200 km closed form");

        const CoastMarchCheck pass = check_coast_march_covers_box(
            b_min_lat, b_min_lon, b_max_lon, config.meters_per_degree_lat,
            config.azimuth_min_deg, config.azimuth_max_deg,
            fail.required_km + 1.0);
        assert(pass.covers && "a give-up above required_km must pass");
        std::puts("PASS: coast-march coverage fails the production box at 100 km");
    }

    // ── Coast-march coverage: cardinal-only window needs exactly the width ─
    // With a single due-west azimuth (bearing 90°) the along-ray span IS the
    // box width; the boundary is sharp on both sides.
    {
        const double b_min_lat = 36.0, b_min_lon = -123.0, b_max_lon = -122.0;
        const double width_km = 1.0 * config.meters_per_degree_lat *
                                std::cos(b_min_lat * M_PI / 180.0) / 1000.0;
        const CoastMarchCheck just_under = check_coast_march_covers_box(
            b_min_lat, b_min_lon, b_max_lon, config.meters_per_degree_lat,
            270.0, 270.0, width_km - 0.5);
        const CoastMarchCheck just_over = check_coast_march_covers_box(
            b_min_lat, b_min_lon, b_max_lon, config.meters_per_degree_lat,
            270.0, 270.0, width_km + 0.5);
        assert(!just_under.covers && "give-up under the box width must fail");
        assert(just_over.covers && "give-up over the box width must pass");
        assert(std::fabs(just_over.required_km - width_km) < 0.01 &&
               "cardinal required span equals the box width");
        std::puts("PASS: cardinal coverage boundary sits exactly at the box width");
    }

    // ── The current config covers the current production box ─────────────
    // Config-driven guard: if either the box in boxes.json grows or
    // coast_march_max_km shrinks, this trips before a run silently would.
    {
        const CoastMarchCheck r = check_coast_march_covers_box(
            36.1442, -123.2651, -121.5431, config.meters_per_degree_lat,
            config.azimuth_min_deg, config.azimuth_max_deg,
            config.coast_march_max_km);
        assert(r.covers &&
               "config coast_march_max_km must cover the production box");
        std::puts("PASS: the shipped config covers the production box");
    }

    std::puts("All preflight tests passed.");
    return 0;
}
