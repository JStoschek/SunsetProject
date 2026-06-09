"""Tests for downsample_2x2 (byte-wise OR bitmask downsample, ADR-0013)."""

from __future__ import annotations

import pytest

from encode_tiles.downsample import downsample_2x2


@pytest.mark.parametrize(
    "pixels,expected",
    [
        pytest.param(
            [b"\x00", b"\x00", b"\x00", b"\x00"],
            b"\x00",
            id="all-transparent",
        ),
        pytest.param(
            # bit 0 clear in all four children → gap at bit 0 survives in result
            [b"\x04", b"\x04", b"\x04", b"\x04"],
            b"\x04",
            id="gap-survives-in-all-children",
        ),
        pytest.param(
            # bit 0 set in one child → gap at bit 0 is closed in result
            [b"\x01", b"\x00", b"\x00", b"\x00"],
            b"\x01",
            id="gap-closed-by-any-child",
        ),
        pytest.param(
            # flag bit (0x80) set in one child → coarse pixel is opaque land
            [b"\x80", b"\x00", b"\x00", b"\x00"],
            b"\x80",
            id="flag-bit-ors",
        ),
        pytest.param(
            # multi-byte pixels: OR is byte-by-byte, not across bytes
            [b"\x01\x80", b"\x00\x00", b"\x02\x00", b"\x00\x00"],
            b"\x03\x80",
            id="multibyte-bytewise-or",
        ),
        pytest.param(
            # all four children have all bits set → result has all bits set
            [b"\xff", b"\xff", b"\xff", b"\xff"],
            b"\xff",
            id="all-bits-set",
        ),
        pytest.param(
            # 10-byte pixels matching production config (77 visibility bits + flag)
            # Two children with disjoint visibility → union in result
            [
                bytes([0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x20]),  # bit 0 + flag
                bytes([0x00, 0, 0, 0, 0, 0x02, 0, 0, 0, 0x20]),  # bit 41 + flag
                bytes(10),
                bytes(10),
            ],
            bytes([0x01, 0, 0, 0, 0, 0x02, 0, 0, 0, 0x20]),
            id="ten-byte-production-disjoint-union",
        ),
    ],
)
def test_downsample_2x2(pixels: list[bytes], expected: bytes) -> None:
    assert downsample_2x2(pixels) == expected
