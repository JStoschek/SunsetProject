// Unit test for AzimuthRangeAccumulator. Uses hardcoded AzimuthSlice objects
// — no DEM tiles or GSHHG data required.
//
// Three-pixel grid (width=3, height=1), three-azimuth sweep (265°, 270°, 275°):
//   pixel[0] = A — visible at all three azimuths → min 265, max 275
//   pixel[1] = B — visible at 270° only         → min 270, max 270
//   pixel[2] = C — never visible                 → NaN, NaN

#include "AzimuthRangeAccumulator.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

AzimuthSlice make_slice(std::vector<bool> visible) {
    AzimuthSlice s;
    s.width   = static_cast<int>(visible.size());
    s.height  = 1;
    s.visible = std::move(visible);
    return s;
}

bool is_nan(float v) { return std::isnan(v); }

}  // namespace

int main() {
    // ── NaN-guard: single visibility sets both min and max ────────────────
    // If only one azimuth is visible, min and max must both equal that azimuth
    // rather than producing NaN from std::min(NaN, az).
    {
        AzimuthRangeAccumulator acc(3);

        // Only pixel B (index 1) is visible at 270°.
        acc.accumulate(make_slice({false, true, false}), 270.0);

        assert(is_nan(acc.min_az_buf[0]) && "A: not yet seen → NaN");
        assert(is_nan(acc.max_az_buf[0]) && "A: not yet seen → NaN");
        assert(acc.min_az_buf[1] == 270.0f && "B: first visibility sets min to 270");
        assert(acc.max_az_buf[1] == 270.0f && "B: first visibility sets max to 270");
        assert(is_nan(acc.min_az_buf[2]) && "C: not visible → NaN");
        assert(is_nan(acc.max_az_buf[2]) && "C: not visible → NaN");
        std::puts("PASS: NaN-guard — first visibility sets both min and max");
    }

    // ── Three-azimuth sweep produces correct min/max range ────────────────
    // Sweep 265°→270°→275°. Pixel A visible at all three; B at 270° only; C never.
    {
        AzimuthRangeAccumulator acc(3);

        acc.accumulate(make_slice({true,  false, false}), 265.0);
        acc.accumulate(make_slice({true,  true,  false}), 270.0);
        acc.accumulate(make_slice({true,  false, false}), 275.0);

        assert(acc.min_az_buf[0] == 265.0f && "A: min azimuth is 265");
        assert(acc.max_az_buf[0] == 275.0f && "A: max azimuth is 275");
        assert(acc.min_az_buf[1] == 270.0f && "B: single visibility → min 270");
        assert(acc.max_az_buf[1] == 270.0f && "B: single visibility → max 270");
        std::puts("PASS: three-azimuth sweep → correct min/max range for A and B");
    }

    // ── Never-visible pixel stays NaN ─────────────────────────────────────
    {
        AzimuthRangeAccumulator acc(3);

        acc.accumulate(make_slice({true, true, false}), 265.0);
        acc.accumulate(make_slice({true, true, false}), 270.0);
        acc.accumulate(make_slice({true, true, false}), 275.0);

        assert(is_nan(acc.min_az_buf[2]) && "C: never visible → min stays NaN");
        assert(is_nan(acc.max_az_buf[2]) && "C: never visible → max stays NaN");
        std::puts("PASS: never-visible pixel stays NaN after full sweep");
    }

    std::puts("ALL PASS");
    return 0;
}
