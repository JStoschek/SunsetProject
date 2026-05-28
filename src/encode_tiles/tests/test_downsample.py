"""Tests for the downsample_2x2 pure function."""

from __future__ import annotations

import pytest

from encode_tiles.downsample import downsample_2x2

TRANSPARENT = (0, 0, 0, 0)


@pytest.mark.parametrize(
    "pixels,expected",
    [
        pytest.param(
            [TRANSPARENT, TRANSPARENT, TRANSPARENT, TRANSPARENT],
            TRANSPARENT,
            id="all-transparent",
        ),
        pytest.param(
            [(180, 90, 0, 200), TRANSPARENT, TRANSPARENT, TRANSPARENT],
            (180, 90, 0, 255),
            id="one-opaque-three-transparent",
        ),
        pytest.param(
            # Equal alpha → plain arithmetic mean of R and G; B always 0
            [(100, 40, 0, 128), (200, 80, 0, 128), (60, 20, 0, 128), (80, 60, 0, 128)],
            (110, 50, 0, 255),
            id="four-equal-alpha",
        ),
        pytest.param(
            # Distinct alphas: naive unweighted mean of R = (240+60)/2 = 150,
            # but weighted: (240*200 + 60*50)/(200+50) = 51000/250 = 204.
            [(240, 10, 0, 200), (60, 90, 0, 50), TRANSPARENT, TRANSPARENT],
            (204, 26, 0, 255),
            id="mixed-alpha-weighted",
        ),
    ],
)
def test_downsample_2x2(
    pixels: list[tuple[int, int, int, int]],
    expected: tuple[int, int, int, int],
) -> None:
    assert downsample_2x2(pixels) == expected
