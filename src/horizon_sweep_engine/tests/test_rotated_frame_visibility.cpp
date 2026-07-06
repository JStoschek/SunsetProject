// Behavioural specification for HorizonSweepEngine::compute_slice at NON-cardinal
// sunset azimuths (the rotated-frame gather, ADR-0007 / ADR-0014). Tests drive the
// engine ONLY through compute_slice with FakeDEM / FakeWater — they never inspect
// running_max_slope, call private methods, or assert on query counts. Assertions
// sit interior to visible/blocked regions (≤1-cell boundary wobble is accepted
// under the forward-sampled grid).
//
// PIPELINE_CONF_PATH is injected by CMake so the tests stay config-driven.
#include "Fakes.h"
#include "PipelineConfig.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }

// A test bounding box on the California coast. Tall enough (~540 rows) that a
// tilted ray has room to move across several latitude rows within the box.
struct Box {
    double min_lat = 37.0;
    double max_lat = 37.05;     // ~540 rows at 1/3 arc-second
    double min_lon = -123.0;
    double max_lon = -122.4;    // ~6480 cols
};

PipelineConfig load_config() { return PipelineConfig::load(PIPELINE_CONF_PATH); }

// Column index for a pixel that sits `east_m` metres east of the meridional
// coast, using the engine's flat-earth anchor (cos of the box's min latitude).
int col_for_east(const PipelineConfig& c, const Box& b, double coast_lon,
                 double east_m) {
    const double cosphi0 = std::cos(deg2rad(b.min_lat));
    const double lon = coast_lon + east_m / (c.meters_per_degree_lat * cosphi0);
    return static_cast<int>(std::lround((lon - b.min_lon) * c.cell_per_degree));
}

bool pixel(const AzimuthSlice& s, int row, int col) {
    return s.visible[static_cast<std::size_t>(row) * s.width + col];
}

}  // namespace

int main() {
    const PipelineConfig config = load_config();
    const Box box;
    const double coast_lon = -122.95;

    // ── Azimuth independence: same cross-section, same visibility ───────
    // Flat sea-level terrain is identical regardless of azimuth, so visibility
    // is a pure function of along-ray distance d. For a meridional coast a pixel
    // sitting `E` metres east of the coast lies at along-ray distance E/sin(beta)
    // (the eastward leg of the ray is the along-ray leg scaled by sin(beta)).
    // Therefore a pixel at eastward E in the 270deg run and a pixel at eastward
    // E*sin(beta) in a tilted run share the SAME along-ray distance and MUST get
    // the same visibility. A row-per-DEM-row engine that treats eastward distance
    // as along-ray distance breaks this near the curvature horizon.
    {
        constexpr double kWest    = 270.0;
        constexpr double kTilted  = 285.0;  // max tilt in range; beta = 105deg
        const double beta_tilted  = std::fmod(kTilted + 180.0, 360.0);
        const double sin_b        = std::sin(deg2rad(beta_tilted));

        FakeDEM   dem([](double, double) { return 0.0f; });  // flat sea level
        FakeWater water(coast_lon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);

        AzimuthSlice west_slice, tilted_slice;
        engine.compute_slice(kWest,   west_slice);
        engine.compute_slice(kTilted, tilted_slice);

        const int row = 200;  // a mid-box row, well inside [0, height)
        // Eastward distances straddling the curvature horizon. The middle three
        // sit in the narrow band (~4410-4566 m) where mistaking eastward distance
        // for along-ray distance flips the verdict; 2000 m and 7000 m bracket it
        // (clearly visible / clearly hidden under either reading).
        for (double east_m : {2000.0, 4450.0, 4500.0, 4550.0, 7000.0}) {
            const int west_col   = col_for_east(config, box, coast_lon, east_m);
            const int tilted_col = col_for_east(config, box, coast_lon, east_m * sin_b);
            assert(pixel(west_slice, row, west_col) ==
                       pixel(tilted_slice, row, tilted_col) &&
                   "matched along-ray distance must give identical visibility");
        }
        std::puts("PASS: azimuth independence at matched along-ray distance");
    }

    // ── Obstruction follows the tilted ray, not the DEM row ─────────────
    // A single localized peak shadows whatever lies DOWNSTREAM ALONG ITS RAY —
    // which, for a tilted azimuth, is a pixel in a DIFFERENT latitude row (the
    // ray drifts south as it runs inland at 285deg). A pixel due-east of the
    // peak in the SAME row is on a different ray that never touches the peak, so
    // it stays visible. An engine that cast one ray per DEM row would get both
    // backwards. This is the crux of the rotated-frame gather (ADR-0007).
    {
        constexpr double kTilted = 285.0;             // beta = 105deg
        const double beta = std::fmod(kTilted + 180.0, 360.0);
        const double sin_b = std::sin(deg2rad(beta));
        const double cos_b = std::cos(deg2rad(beta));
        const double mpd   = config.meters_per_degree_lat;
        const double mE    = mpd * std::cos(deg2rad(box.min_lat));

        // A 500 m peak ~1.5 km east of the coast, mid-box, localized to ~±44 m.
        const double lat_pk = 37.02;
        const double lon_pk = coast_lon + 1500.0 / mE;
        FakeDEM dem([=](double lat, double lon) {
            return (std::fabs(lon - lon_pk) < 0.0005 &&
                    std::fabs(lat - lat_pk) < 0.0005) ? 500.0f : 0.0f;
        });
        FakeWater water(coast_lon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kTilted, slice);

        // Local-frame coordinates of the peak relative to the box anchor.
        const double E_pk = (lon_pk - box.min_lon) * mE;
        const double N_pk = (lat_pk - box.min_lat) * mpd;

        // Downstream observer: 1200 m further along the SAME ray (east + south).
        const double E_down = E_pk + 1200.0 * sin_b;
        const double N_down = N_pk + 1200.0 * cos_b;
        const int down_row = static_cast<int>(std::lround(N_down / mpd * config.cell_per_degree));
        const int down_col = static_cast<int>(std::lround(E_down / mE  * config.cell_per_degree));

        // Same-row sibling: 700 m further east at the peak's own latitude, which
        // lands on a ray that runs south of the peak and never hits it.
        const int peak_row = static_cast<int>(std::lround(N_pk / mpd * config.cell_per_degree));
        const int east_col = static_cast<int>(std::lround((E_pk + 700.0) / mE * config.cell_per_degree));

        assert(!pixel(slice, down_row, down_col) &&
               "peak must shadow the pixel downstream along its tilted ray");
        assert(pixel(slice, peak_row, east_col) &&
               "a same-row pixel east of the peak is on another ray and stays visible");
        std::puts("PASS: obstruction follows the tilted ray, not the DEM row");
    }

    // ── The ray partition leaves no holes ───────────────────────────────
    // round(perp/s) assigns every in-bounds pixel to exactly one ray, so a
    // tilted slice must have no unwritten gaps. On flat terrain each row's
    // visible pixels form one band — ocean to the west, then visible land out
    // to the curvature horizon. A hole in the gather (an unwritten pixel, left
    // false) would puncture that band's INTERIOR. The band's edges may wobble
    // by ~1 cell where adjacent rays quantise their coast crossings and
    // horizons differently (ADR-0014), so we assert only the interior — a few
    // columns clear of each edge — is solid.
    {
        constexpr double kTilted = 285.0;
        FakeDEM   dem([](double, double) { return 0.0f; });  // flat sea level
        FakeWater water(coast_lon);
        HorizonSweepEngine engine(dem, water, config,
                                  box.min_lat, box.max_lat,
                                  box.min_lon, box.max_lon);
        AzimuthSlice slice;
        engine.compute_slice(kTilted, slice);

        for (int row : {40, 200, 360, 500}) {
            int first = -1, last = -1;
            for (int col = 0; col < slice.width; ++col) {
                if (pixel(slice, row, col)) {
                    if (first < 0) first = col;
                    last = col;
                }
            }
            assert(first >= 0 && "each row has a visible coastal band");
            assert(last - first > 10 && "the visible band spans many columns");
            for (int col = first + 3; col <= last - 3; ++col) {
                assert(pixel(slice, row, col) &&
                       "the band interior is solid: the partition leaves no holes");
            }
        }
        std::puts("PASS: the ray partition leaves no holes across a tilted slice");
    }

    std::puts("ALL PASS");
    return 0;
}
