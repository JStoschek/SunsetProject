"""Tests for the downsample_2x2 pure function (union/hull semantics)."""

from __future__ import annotations

import pytest

from encode_tiles.downsample import downsample_2x2

TRANSPARENT = (0, 0, 0, 0)
SENTINEL = (255, 0, 0, 255)  # never-visible land: empty interval (R > G)


@pytest.mark.parametrize(
    "pixels,expected",
    [
        pytest.param(
            [TRANSPARENT, TRANSPARENT, TRANSPARENT, TRANSPARENT],
            TRANSPARENT,
            id="all-transparent",
        ),
        pytest.param(
            # A single visible interval survives unchanged (water excluded).
            [(53, 174, 0, 200), TRANSPARENT, TRANSPARENT, TRANSPARENT],
            (53, 174, 0, 255),
            id="one-visible-three-transparent",
        ),
        pytest.param(
            # Hull of four visible intervals: R = min R, G = max G.
            [(100, 140, 0, 128), (60, 90, 0, 128), (80, 200, 0, 128), (70, 120, 0, 128)],
            (60, 200, 0, 255),
            id="four-visible-hull",
        ),
        pytest.param(
            # The never-visible sentinel must NOT drag the hull: the visible
            # foreshore interval is preserved (this is the coast-line fix).
            [(53, 174, 0, 255), SENTINEL, TRANSPARENT, TRANSPARENT],
            (53, 174, 0, 255),
            id="visible-plus-sentinel-keeps-visible",
        ),
        pytest.param(
            # Every covered land sub-pixel is never-visible → stay sentinel.
            [SENTINEL, SENTINEL, TRANSPARENT, TRANSPARENT],
            SENTINEL,
            id="all-sentinel",
        ),
        pytest.param(
            # Alpha is ignored for the hull (presence, not coverage-weighted).
            [(240, 250, 0, 10), (10, 20, 0, 255), TRANSPARENT, TRANSPARENT],
            (10, 250, 0, 255),
            id="alpha-ignored-for-hull",
        ),
    ],
)
def test_downsample_2x2(
    pixels: list[tuple[int, int, int, int]],
    expected: tuple[int, int, int, int],
) -> None:
    assert downsample_2x2(pixels) == expected
