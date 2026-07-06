// Regression: the march and the gather must agree on which ray owns a point —
// and the diagnostic RayTrace must dump exactly that ray.
//
// The engine computes everything in the ADR-0007 rotated flat frame (anchored
// at cos(min_lat)). The gather assigns each output pixel to ray round(perp/s);
// since the ADR-0014 amendment the march steps STRAIGHT in that frame
// (E += s·sin b, N += s·cos b), so the ray that owns a pixel is precisely the
// ray whose march passes within half a sample spacing of it, and the trace
// shows the very samples the map verdict was gathered from.
//
// This was not always true: the march originally advanced longitude by the
// *local* cos(lat) per step, bending each marched path off its frame line by
// hundreds of metres on a tall box (~250 m here; ~220 m at production extent —
// see the 2026-07-06 trace_ray investigation at Thornton Beach, where the
// trace said "visible" while the map said "not visible" because the pixel
// inherited its verdict from a ray marching 224 m away). This test pins the
// consistency contract three ways:
//   1. the trace resolves the SAME ray index the gather assigns the target to;
//   2. the resolved ray's coast crossing lies ON its frame line (perp = j·s) —
//      the march never drifted off it;
//   3. that line passes within half a sample spacing of the target.
// Reintroducing local-cos stepping fails check 2 by ~100 m on this box.
//
// Uses CHECK (not assert) so the test still bites in NDEBUG/Release builds.
// PIPELINE_CONF_PATH is injected by CMake so the test stays config-driven.
#include "Fakes.h"
#include "PipelineConfig.h"

#include <climits>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
            return 1;                                                 \
        }                                                             \
    } while (0)
}  // namespace

int main() {
    const PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);

    // A 1-degree-tall box: the target sits ~0.9 deg north of the frame anchor,
    // so cos(lat) near the target differs sharply from cos(min_lat) — the
    // geometry where local-cos stepping drifted worst.
    const double min_lat = 37.0, max_lat = 38.0;
    const double min_lon = -123.0, max_lon = -122.0;
    const double coast_lon  = -122.8;
    const double target_lat = 37.90, target_lon = -122.20;
    const double azimuth    = 285.0;  // bearing 105 deg: sin_b dominant, real tilt

    FakeDEM   dem([](double, double) { return 0.0f; });  // flat sea level
    FakeWater water(coast_lon);
    HorizonSweepEngine engine(dem, water, config,
                              min_lat, max_lat, min_lon, max_lon);

    long   resolved_j = LONG_MIN;
    double cross_lat = 0.0, cross_lon = 0.0;
    HorizonSweepEngine::RayTrace trace;
    trace.enabled      = true;
    trace.target_lat   = target_lat;
    trace.target_lon   = target_lon;
    trace.steps_before = 0;
    trace.steps_after  = 0;
    trace.on_resolved  = [&](long j, double la, double lo) {
        resolved_j = j; cross_lat = la; cross_lon = lo;
    };
    engine.set_trace(trace);

    AzimuthSlice slice;
    engine.compute_slice(azimuth, slice);

    CHECK(resolved_j != LONG_MIN,
          "trace must resolve a ray for an in-box target");

    // Frame quantities, exactly as the engine computes them.
    const double mpd  = config.meters_per_degree_lat;
    const double mE   = mpd * std::cos(deg2rad(min_lat));
    const double s    = (config.sample_spacing_arcsec / 3600.0) * mpd;
    const double bearing = std::fmod(azimuth + 180.0, 360.0);
    const double sin_b   = std::sin(deg2rad(bearing));
    const double cos_b   = std::cos(deg2rad(bearing));
    auto perp_of = [&](double lat, double lon) {
        const double E = (lon - min_lon) * mE;
        const double N = (lat - min_lat) * mpd;
        return E * cos_b - N * sin_b;
    };

    // 1. The trace ray IS the gather ray: the map verdict for the target pixel
    //    is read from this exact ray.
    const long gather_j = std::lround(perp_of(target_lat, target_lon) / s);
    std::printf("resolved ray j=%ld  gather ray j=%ld\n", resolved_j, gather_j);
    CHECK(resolved_j == gather_j,
          "the traced ray must be the ray the gather assigns the target to");

    // 2. The march stayed on its frame line: the crossing the engine actually
    //    marched to has perp = j*s. Local-cos stepping drifts ~100 m off the
    //    line by the coast here; allow only accumulated float error.
    const double cross_off_m =
        std::fabs(perp_of(cross_lat, cross_lon) - resolved_j * s);
    std::printf("crossing off its frame line by %.6f m\n", cross_off_m);
    CHECK(cross_off_m < 0.01,
          "the marched coast crossing must lie on the ray's frame line");

    // 3. That line passes within half a sample spacing of the target — the
    //    nearest-neighbour guarantee of the Visibility Sample Grid.
    const double target_off_m =
        std::fabs(perp_of(target_lat, target_lon) - resolved_j * s);
    std::printf("target off the ray's frame line by %.2f m (s/2 = %.2f m)\n",
                target_off_m, 0.5 * s);
    CHECK(target_off_m <= 0.5 * s + 1e-6,
          "the traced ray must pass within half a sample spacing of the target");

    std::puts("PASS: march, gather, and trace agree on ray ownership");
    std::puts("ALL PASS");
    return 0;
}
