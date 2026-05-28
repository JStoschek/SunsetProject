"""Table-driven tests for encode_pixel (ADR-0003 wire format)."""

from __future__ import annotations

import math

import pytest

from encode_tiles.encoder import encode_pixel

# ADR-0003 decode formula: az = channel / 255 * 160 + 200
_PRECISION_DEG = 0.6


def _decode(channel: int) -> float:
    return channel / 255 * 160 + 200


def test_known_good_roundtrip() -> None:
    """233° and 309° survive encode→decode within 0.6° (tracer bullet)."""
    r, g, b, a = encode_pixel(233.0, 309.0)
    assert _decode(r) == pytest.approx(233.0, abs=_PRECISION_DEG)
    assert _decode(g) == pytest.approx(309.0, abs=_PRECISION_DEG)
    assert b == 0
    assert a == 255


@pytest.mark.parametrize(
    "min_az, max_az",
    [
        (200.0, 360.0),  # window edges
        (233.0, 309.0),  # known-good (also tested standalone above)
    ],
)
def test_valid_roundtrip(min_az: float, max_az: float) -> None:
    """Valid pairs encode and decode within 0.6° precision, B=0, A=255."""
    r, g, b, a = encode_pixel(min_az, max_az)
    assert _decode(r) == pytest.approx(min_az, abs=_PRECISION_DEG)
    assert _decode(g) == pytest.approx(max_az, abs=_PRECISION_DEG)
    assert b == 0
    assert a == 255


def test_both_nan_returns_transparent() -> None:
    """Both NaN → fully transparent (0, 0, 0, 0)."""
    assert encode_pixel(float("nan"), float("nan")) == (0, 0, 0, 0)


@pytest.mark.parametrize(
    "min_az, max_az",
    [
        (float("nan"), 309.0),   # min NaN, max valid
        (233.0, float("nan")),   # min valid, max NaN
    ],
)
def test_one_nan_raises(min_az: float, max_az: float) -> None:
    """Exactly one NaN raises ValueError including both offending values."""
    with pytest.raises(ValueError, match="nan"):
        encode_pixel(min_az, max_az)


def test_min_greater_than_max_raises() -> None:
    """min_az > max_az raises ValueError including both offending values."""
    with pytest.raises(ValueError) as exc_info:
        encode_pixel(310.0, 280.0)
    msg = str(exc_info.value)
    assert "310.0" in msg
    assert "280.0" in msg


def test_out_of_range_low_raises() -> None:
    """min_az < 200 raises ValueError including both offending values."""
    with pytest.raises(ValueError) as exc_info:
        encode_pixel(199.9, 300.0)
    msg = str(exc_info.value)
    assert "199.9" in msg
    assert "300.0" in msg


def test_out_of_range_high_raises() -> None:
    """max_az > 360 raises ValueError including both offending values."""
    with pytest.raises(ValueError) as exc_info:
        encode_pixel(250.0, 360.1)
    msg = str(exc_info.value)
    assert "250.0" in msg
    assert "360.1" in msg
