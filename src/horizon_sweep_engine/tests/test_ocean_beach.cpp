// Ocean Beach must not be blacked out (PRD engine-forward-sampled-grid §8.2).
//
// The known-visible Ocean Beach / Great Highway band sits a few hundred metres
// inland of a low beach berm. A past perf change (b350d3e) silently blacked
// the beach out at its NW summer-sunset azimuth; this test pins the band
// explicitly, with the berm modelled BELOW eye height (the real profile: an
// observer on the Great Highway plainly sees the ocean horizon over the sand).
//
// Geometry: FakeWater meridional coast at the Ocean Beach shoreline, FakeDEM
// with a sub-eye-height berm on the first ~60 m of land, flat behind. Sunset
// azimuth 300° (NW summer sunset, bearing 120°). The whole band — several
// rows x every column from 100 m to 400 m inland — must be visible. All
// distances are safely interior to the visible region (≤1-cell boundary
// wobble is accepted under the forward-sampled grid, ADR-0014).
//
// Uses CHECK (not assert) so the test still bites in NDEBUG/Release builds.
// PIPELINE_CONF_PATH is injected by CMake so the test stays config-driven.
#include "Fakes.h"
#include "PipelineConfig.h"

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

// Sunset azimuth under test: NW summer sunset over Ocean Beach.
constexpr double kAz = 300.0;

// The Ocean Beach shoreline (Outer Sunset, San Francisco).
constexpr double kCoastLon = -122.511;

// Test box: ~10 rows around the Great Highway latitude band, western edge in
// the open Pacific, extending east well past the asserted band.
struct Box {
    double min_lat = 37.745;
    double max_lat = 37.7459;
    double min_lon = -123.0;
    double max_lon = -122.40;
};

bool pixel(const AzimuthSlice& s, int row, int col) {
    return s.visible[static_cast<std::size_t>(row) * s.width + col];
}

}  // namespace

int main() {
    const PipelineConfig config = PipelineConfig::load(PIPELINE_CONF_PATH);
    const Box box;

    const double mpd    = config.meters_per_degree_lat;
    const double mE     = mpd * std::cos(deg2rad(box.min_lat));
    const double offset = config.horizon_reference_offset_m;
    const double eye    = config.observer_eye_height_m;
    const double c      = (1.0 - 2.0 * config.refraction_coefficient_k)
                          / (2.0 * config.earth_radius_m);
    const double sin_b  = std::sin(deg2rad(std::fmod(kAz + 180.0, 360.0)));

    // Beach berm: sub-eye-height sand on the first ~60 m of land.
    const double berm_h       = 0.75 * eye;
    const double berm_depth_m = 60.0;

    // The asserted Great Highway band: 100–400 m inland (eastward).
    const double band_lo_m = 100.0, band_hi_m = 400.0;

    // ── Setup sanity (closed form, config-driven) ────────────────────────
    // Worst-case berm obstruction slope: the earliest possible berm sample
    // (d = offset). Worst-case band observer: the far edge of the band at the
    // slant distance 400 m east / sin(bearing), plus a sample of quantisation.
    const double berm_slope = (berm_h - offset * offset * c) / offset;
    const double d_far      = offset + band_hi_m / sin_b + 15.0;
    const double band_slope = (eye - d_far * d_far * c) / d_far;
    CHECK(config.coast_obstruction_skip_m >= 0.0, "setup: config loaded");
    CHECK(band_slope > berm_slope * 1.05,
          "setup: the band must clear the sub-eye-height berm with margin");

    // ── Run the slice ─────────────────────────────────────────────────────
    FakeDEM dem([&](double, double lon) -> float {
        const double east_m = (lon - kCoastLon) * mE;
        if (east_m >= 0.0 && east_m <= berm_depth_m)
            return static_cast<float>(berm_h);
        return 0.0f;
    });
    FakeWater water(kCoastLon);
    HorizonSweepEngine engine(dem, water, config,
                              box.min_lat, box.max_lat,
                              box.min_lon, box.max_lon);
    AzimuthSlice slice;
    engine.compute_slice(kAz, slice);

    // ── The whole band is visible on every interior row ───────────────────
    const double cell_deg = 1.0 / config.cell_per_degree;
    const int col_lo = static_cast<int>(
        std::lround((kCoastLon + band_lo_m / mE - box.min_lon) / cell_deg));
    const int col_hi = static_cast<int>(
        std::lround((kCoastLon + band_hi_m / mE - box.min_lon) / cell_deg));
    CHECK(col_lo > 0 && col_hi < slice.width && col_hi > col_lo + 10,
          "setup: the band spans many columns inside the box");

    for (int row = 2; row <= 7; ++row) {
        for (int col = col_lo; col <= col_hi; ++col) {
            CHECK(pixel(slice, row, col),
                  "Ocean Beach / Great Highway band must be visible at az 300 "
                  "(a shift in the coast crossing or along_c blacked it out)");
        }
    }
    std::puts("PASS: the Ocean Beach / Great Highway band is visible at az = 300");

    // ── Open water stays dark ─────────────────────────────────────────────
    // Cells clearly seaward of the shoreline must stay false. (Cells within a
    // few columns of the crossing may wobble by one cell; stay clear of them.)
    const int coast_col = static_cast<int>(
        std::lround((kCoastLon - box.min_lon) / cell_deg));
    for (int row = 2; row <= 7; ++row)
        for (int col = 0; col <= coast_col - 10; ++col)
            CHECK(!pixel(slice, row, col),
                  "open Pacific west of Ocean Beach must stay not-visible");
    std::puts("PASS: the open Pacific west of the shoreline stays dark");

    std::puts("ALL PASS");
    return 0;
}
