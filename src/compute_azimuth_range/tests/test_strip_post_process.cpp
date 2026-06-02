// Behavioral tests for apply_water_mask (StripPostProcess.h).
// Three behaviors:
//   1. Water pixel + finite range   → transparent  (NaN, NaN)
//   2. Land pixel + NaN (no range)  → never-visible sentinel (+inf, -inf)
//   3. Land pixel + finite range    → unchanged

#include "StripPostProcess.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

bool is_nan(float v) { return std::isnan(v); }
bool is_pos_inf(float v) { return std::isinf(v) && v > 0; }
bool is_neg_inf(float v) { return std::isinf(v) && v < 0; }

// Build a 1×1 StripResult with explicit min/max values.
StripResult make_result(float min_az, float max_az) {
    StripResult r;
    r.width  = 1;
    r.height = 1;
    r.min_az_buf = {min_az};
    r.max_az_buf = {max_az};
    return r;
}

const float NaN     = std::numeric_limits<float>::quiet_NaN();
const float POS_INF = std::numeric_limits<float>::infinity();
const float NEG_INF = -std::numeric_limits<float>::infinity();

}  // namespace

int main() {
    // ── 1. Water pixel + finite range → transparent (NaN, NaN) ──────────────
    // This is the guard: the sweep gave a pixel a finite Azimuth Range, but the
    // Ocean Mask says it is water — it must be forced transparent.
    {
        StripResult r = make_result(265.0f, 275.0f);
        auto always_water = [](double /*lat*/, double /*lon*/) { return true; };
        apply_water_mask(r, /*strip_min_lat=*/37.0, /*min_lon=*/-122.5,
                         /*cell_deg=*/1.0 / 3600.0, always_water);

        assert(is_nan(r.min_az_buf[0]) && "water+finite: min_az must be NaN");
        assert(is_nan(r.max_az_buf[0]) && "water+finite: max_az must be NaN");
        std::puts("PASS: water pixel + finite range → transparent (NaN, NaN)");
    }

    // ── 2. Land NaN → never-visible sentinel (+inf, -inf) ───────────────────
    {
        StripResult r = make_result(NaN, NaN);
        auto never_water = [](double /*lat*/, double /*lon*/) { return false; };
        apply_water_mask(r, 37.0, -122.5, 1.0 / 3600.0, never_water);

        assert(is_pos_inf(r.min_az_buf[0]) && "land NaN: min_az must be +inf");
        assert(is_neg_inf(r.max_az_buf[0]) && "land NaN: max_az must be -inf");
        std::puts("PASS: land NaN → never-visible sentinel (+inf, -inf)");
    }

    // ── 3. Finite land range → unchanged ────────────────────────────────────
    {
        StripResult r = make_result(268.0f, 272.0f);
        auto never_water = [](double /*lat*/, double /*lon*/) { return false; };
        apply_water_mask(r, 37.0, -122.5, 1.0 / 3600.0, never_water);

        assert(r.min_az_buf[0] == 268.0f && "finite land: min_az must be preserved");
        assert(r.max_az_buf[0] == 272.0f && "finite land: max_az must be preserved");
        std::puts("PASS: finite land range preserved unchanged");
    }

    std::puts("ALL PASS");
    return 0;
}
