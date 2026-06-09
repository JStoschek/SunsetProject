"""Write the bitmask pyramid as XYZ-scheme gzip-compressed .bin tiles (ADR-0013)."""

from __future__ import annotations

import gzip
from pathlib import Path

import numpy as np

from encode_tiles.mosaic import TILE_SIZE, BaseMosaic


def _write_zoom_level_tiles(
    data: np.ndarray,
    zoom: int,
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
    output_dir: Path,
) -> int:
    """Split a mosaic into gzip-compressed .bin tiles. Returns count written.

    Tiles where every byte is zero (all pixels transparent) are skipped.
    """
    count = 0
    for ty in range(tile_ymin, tile_ymax + 1):
        row0 = (ty - tile_ymin) * TILE_SIZE
        row1 = row0 + TILE_SIZE
        for tx in range(tile_xmin, tile_xmax + 1):
            col0 = (tx - tile_xmin) * TILE_SIZE
            col1 = col0 + TILE_SIZE
            tile = data[row0:row1, col0:col1, :]
            if not np.any(tile):
                continue
            tile_dir = output_dir / str(zoom) / str(tx)
            tile_dir.mkdir(parents=True, exist_ok=True)
            with gzip.open(tile_dir / f"{ty}.bin", "wb") as f:
                f.write(tile.tobytes())
            count += 1
    return count


def _byteor_downsample(data: np.ndarray) -> np.ndarray:
    """Halve a multi-channel uint8 raster via byte-wise OR of 2×2 blocks."""
    h, w, c = data.shape
    b = data.reshape(h // 2, 2, w // 2, 2, c)
    return b[:, 0, :, 0, :] | b[:, 0, :, 1, :] | b[:, 1, :, 0, :] | b[:, 1, :, 1, :]


def _pad_for_downsample(
    data: np.ndarray,
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
) -> tuple[np.ndarray, int, int, int, int]:
    """Pad the mosaic with transparent tile-strips so that 2×2 parent groups
    at the next-coarser zoom align with mosaic edges.
    """
    pad_left = TILE_SIZE if tile_xmin % 2 != 0 else 0
    pad_right = TILE_SIZE if tile_xmax % 2 != 1 else 0
    pad_top = TILE_SIZE if tile_ymin % 2 != 0 else 0
    pad_bottom = TILE_SIZE if tile_ymax % 2 != 1 else 0

    if pad_left or pad_right or pad_top or pad_bottom:
        data = np.pad(
            data,
            ((pad_top, pad_bottom), (pad_left, pad_right), (0, 0)),
            mode="constant",
            constant_values=0,
        )
        if pad_left:
            tile_xmin -= 1
        if pad_right:
            tile_xmax += 1
        if pad_top:
            tile_ymin -= 1
        if pad_bottom:
            tile_ymax += 1
    return data, tile_xmin, tile_xmax, tile_ymin, tile_ymax


def build_pyramid(base: BaseMosaic, min_zoom: int, output_dir: Path) -> None:
    """Walk zooms top-down from base.zoom to min_zoom, writing tiles and
    downsampling 2×2 between levels. Only one level's mosaic is resident.
    """
    if min_zoom > base.zoom:
        raise ValueError(
            f"min_zoom ({min_zoom}) must be <= base zoom ({base.zoom})"
        )

    data = base.data
    xmin, xmax = base.tile_xmin, base.tile_xmax
    ymin, ymax = base.tile_ymin, base.tile_ymax

    for z in range(base.zoom, min_zoom - 1, -1):
        _write_zoom_level_tiles(data, z, xmin, xmax, ymin, ymax, output_dir)
        if z == min_zoom:
            break
        data, xmin, xmax, ymin, ymax = _pad_for_downsample(
            data, xmin, xmax, ymin, ymax
        )
        data = _byteor_downsample(data)
        xmin, xmax = xmin // 2, xmax // 2
        ymin, ymax = ymin // 2, ymax // 2
