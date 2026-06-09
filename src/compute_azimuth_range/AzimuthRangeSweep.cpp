#include "AzimuthRangeSweep.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include "AzimuthRangeAccumulator.h"
#include "BitLayout.h"

StripResult sweep_strip(StripSources&         sources,
                        const PipelineConfig& config,
                        ThreadPool&           pool,
                        double strip_min_lat, double strip_max_lat,
                        double min_lon,       double max_lon) {
    const int nw = pool.worker_count();

    // One engine per worker (each has its own profile_ buffer) bound to that
    // worker's own elevation/coast sources.
    std::vector<std::unique_ptr<HorizonSweepEngine>> engines;
    engines.reserve(nw);
    for (int i = 0; i < nw; ++i)
        engines.push_back(std::make_unique<HorizonSweepEngine>(
            sources.dem_for_worker(i), sources.ocean_for_worker(i),
            config, strip_min_lat, strip_max_lat, min_lon, max_lon));

    // Size accumulator from engine dimensions — no serial slice needed.
    const int strip_w = engines[0]->width();
    const int strip_h = engines[0]->height();

    // Build ordered azimuth list.
    std::vector<double> azimuths;
    for (double az = config.azimuth_min_deg;
         az <= config.azimuth_max_deg + 0.5 * config.azimuth_step_deg;
         az += config.azimuth_step_deg)
        azimuths.push_back(az);

    // Shared accumulator + fold mutex.  The packing math comes from the single
    // BitLayout wire contract (ADR-0013), derived from the sweep window/step.
    const BitLayout layout = BitLayout::from_config(
        config.azimuth_min_deg, config.azimuth_max_deg, config.azimuth_step_deg);
    AzimuthRangeAccumulator acc(
        static_cast<std::size_t>(strip_w) * strip_h, layout);
    std::mutex acc_mu;

    // Shared azimuth counter: workers claim azimuths atomically.
    std::atomic<int> next_az{0};

    // One task per worker; each claims azimuths until none remain.
    std::vector<std::function<void()>> tasks;
    tasks.reserve(nw);
    for (int w = 0; w < nw; ++w) {
        HorizonSweepEngine* engine = engines[w].get();
        tasks.push_back([&, engine]() {
            AzimuthSlice slice;
            int idx;
            while ((idx = next_az.fetch_add(1, std::memory_order_relaxed))
                   < static_cast<int>(azimuths.size())) {
                engine->compute_slice(azimuths[idx], slice);
                std::lock_guard<std::mutex> lock(acc_mu);
                acc.accumulate(slice, azimuths[idx]);
            }
        });
    }

    pool.run_batch(std::move(tasks));

    return { std::move(acc.mask), strip_w, strip_h, layout.bytes_per_pixel };
}
