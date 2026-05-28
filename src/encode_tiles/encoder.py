"""encode_pixel: pure function mapping azimuth range to RGBA per ADR-0003."""

from __future__ import annotations

import math

AZ_OFFSET: float = 200.0
AZ_RANGE: float = 160.0


def encode_pixel(min_az: float, max_az: float) -> tuple[int, int, int, int]:
    """Return (R, G, B, A) encoding [min_az, max_az] per ADR-0003.

    Both NaN → (0, 0, 0, 0).  One NaN or out-of-range or min > max → ValueError.
    """
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
