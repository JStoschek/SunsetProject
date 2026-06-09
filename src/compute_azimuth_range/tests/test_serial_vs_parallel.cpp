// Serial-vs-parallel gate test.
//
// Runs sweep_strip over a small synthetic bbox twice — once with worker_threads=1
// (serial) and once with worker_threads=4 (parallel) — and asserts byte-for-byte
// identical packed bitmask buffers.  Uses FakeDEM (flat 0 m terrain) and
// FakeCoast (meridional coast), so the test needs no file I/O and no real DEM
// or GSHHG data.
//
// This is the agreed regression gate against the class of silent-corruption bugs
// illustrated by b350d3e / dd441bc: a change to the parallel sweep that produces
// different pixel values for any azimuth completion order must fail here.
//
// PIPELINE_CONF_PATH is injected by CMake.

#include "AzimuthRangeSweep.h"
#include "Fakes.h"
#include "PipelineConfig.h"
#include "ThreadPool.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

// Small box: 3 rows × ~32 cols at 1/3 arc-second covering Ocean Beach area.
// Coast at −122.511° (west edge of box), box extends 3 km east.
constexpr double kMinLat   = 37.744;
constexpr double kMaxLat   = 37.747;   // ≈ 3 rows at full res; test uses low res
constexpr double kMinLon   = -122.515;
constexpr double kMaxLon   = -122.480;
constexpr double kCoastLon = -122.511;

PipelineConfig make_config(int worker_threads) {
    PipelineConfig cfg = PipelineConfig::load(PIPELINE_CONF_PATH);
    // Narrow azimuth sweep to 5 slices so the test is fast.
    cfg.azimuth_min_deg  = 265.0;
    cfg.azimuth_max_deg  = 269.0;
    cfg.azimuth_step_deg = 1.0;
    cfg.worker_threads   = worker_threads;
    return cfg;
}

// Hands every worker the same stateless fake (FakeDEM/FakeCoast are inherently
// thread-safe), so this gate isolates the parallel sweep's determinism from the
// frozen-cache machinery (which is unit-tested separately).
struct SharedStripSources : StripSources {
    ElevationSource& dem;
    CoastlineFinder& ocean;
    SharedStripSources(ElevationSource& d, CoastlineFinder& o) : dem(d), ocean(o) {}
    ElevationSource& dem_for_worker(int)   override { return dem; }
    CoastlineFinder& ocean_for_worker(int) override { return ocean; }
};

}  // namespace

int main() {
    FakeDEM   dem([](double, double) { return 0.0f; });
    FakeCoast coast(kCoastLon);
    SharedStripSources sources(dem, coast);

    const PipelineConfig cfg1 = make_config(1);
    const PipelineConfig cfg4 = make_config(4);

    ThreadPool pool1(1);
    ThreadPool pool4(4);

    const StripResult serial   = sweep_strip(sources, cfg1, pool1,
                                             kMinLat, kMaxLat, kMinLon, kMaxLon);
    const StripResult parallel = sweep_strip(sources, cfg4, pool4,
                                             kMinLat, kMaxLat, kMinLon, kMaxLon);

    assert(serial.width           == parallel.width);
    assert(serial.height          == parallel.height);
    assert(serial.bytes_per_pixel == parallel.bytes_per_pixel);
    assert(serial.mask.size()     == parallel.mask.size());

    int mismatches = 0;
    for (std::size_t b = 0; b < serial.mask.size(); ++b) {
        if (serial.mask[b] != parallel.mask[b]) {
            std::fprintf(stderr, "  byte %zu: serial 0x%02x != parallel 0x%02x\n",
                         b, serial.mask[b], parallel.mask[b]);
            ++mismatches;
        }
    }
    assert(mismatches == 0 && "serial and parallel outputs differ");
    std::puts("PASS: serial (1 thread) and parallel (4 threads) produce identical bitmask buffers");
    return 0;
}
