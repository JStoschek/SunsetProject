#pragma once
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "HorizonSweepEngine.h"

/// Accumulates the per-pixel azimuth range over a sweep of AzimuthSlices.
/// Call accumulate() once per azimuth in ascending order; read min_az_buf and
/// max_az_buf when the sweep is complete.  Both buffers are initialised to NaN
/// so pixels that are never visible remain NaN throughout.
struct AzimuthRangeAccumulator {
    std::vector<float> min_az_buf;
    std::vector<float> max_az_buf;

    explicit AzimuthRangeAccumulator(std::size_t n)
        : min_az_buf(n, std::numeric_limits<float>::quiet_NaN()),
          max_az_buf(n, std::numeric_limits<float>::quiet_NaN()) {}

    void accumulate(const AzimuthSlice& slice, double az) {
        const float az_f = static_cast<float>(az);
        for (std::size_t i = 0; i < slice.visible.size(); ++i) {
            if (slice.visible[i]) {
                if (std::isnan(min_az_buf[i])) {
                    min_az_buf[i] = max_az_buf[i] = az_f;
                } else {
                    if (az_f < min_az_buf[i]) min_az_buf[i] = az_f;
                    if (az_f > max_az_buf[i]) max_az_buf[i] = az_f;
                }
            }
        }
    }
};
