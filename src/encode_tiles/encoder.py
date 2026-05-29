"""encode_pixel: pure function mapping azimuth range to RGBA per ADR-0003."""

from __future__ import annotations

import math

AZ_OFFSET: float = 200.0
AZ_RANGE: float = 160.0


def encode_pixel(min_az: float, max_az: float) -> tuple[int, int, int, int]:
    """Return (R, G, B, A) encoding [min_az, max_az] per ADR-0003.

    Three-state input (matches compute_azimuth_range output):
      - Both NaN → (0, 0, 0, 0) — ocean or DEM-no-data, transparent.
      - (+inf, -inf) → (255, 0, 0, 255) — land that never sees the sunset
        at any swept azimuth. R=255, G=0 makes the frontend's
        `R ≤ encoded_az ≤ G` check unsatisfiable for any az, so the pixel
        is opaque-black at every date.
      - Valid finite pair with min ≤ max → normal RGBA encoding.
    Mixed NaN, NaN with non-NaN, out-of-range, or min > max → ValueError.
    """
    if math.isinf(min_az) and math.isinf(max_az) and min_az > 0 and max_az < 0:
        # Sentinel: land cell with an empty visible-azimuth range.
        return (255, 0, 0, 255)

    min_nan = math.isnan(min_az)
    max_nan = math.isnan(max_az)

    if min_nan and max_nan:
        return (0, 0, 0, 0)

    if min_nan or max_nan:
        raise ValueError(
            f"encode_pixel: exactly one of min_az/max_az is NaN: "
            f"min_az={min_az}, max_az={max_az}"
        )

    if min_az < AZ_OFFSET:
        raise ValueError(
            f"encode_pixel: min_az out of range [200, 360]: "
            f"min_az={min_az}, max_az={max_az}"
        )

    if max_az > AZ_OFFSET + AZ_RANGE:
        raise ValueError(
            f"encode_pixel: max_az out of range [200, 360]: "
            f"min_az={min_az}, max_az={max_az}"
        )

    if min_az > max_az:
        raise ValueError(
            f"encode_pixel: min_az > max_az: min_az={min_az}, max_az={max_az}"
        )

    r = round((min_az - AZ_OFFSET) / AZ_RANGE * 255)
    g = round((max_az - AZ_OFFSET) / AZ_RANGE * 255)
    return (r, g, 0, 255)
