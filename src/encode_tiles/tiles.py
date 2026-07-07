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
    compress_level: int,
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
            with gzip.open(
                tile_dir / f"{ty}.bin", "wb", compresslevel=compress_level
            ) as f:
                f.write(tile.tobytes())
            count += 1
    return count


def _byteor_downsample(data: np.ndarray) -> np.ndarray:
    """Halve a multi-channel uint8 raster via byte-wise OR of 2×2 blocks."""
    h, w, c = data.shape
    b = data.reshape(h // 2, 2, w // 2, 2, c)
    return b[:, 0, :, 0, :] | b[:, 0, :, 1, :] | b[:, 1, :, 0, :] | b[:, 1, :, 1, :]


def _downsample_level(
    data: np.ndarray,
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
) -> tuple[np.ndarray, int, int, int, int]:
    """Produce the next-coarser mosaic and its tile bounds via 2×2 byte-OR.

    Coarser 2×2 parent groups must align to the tile grid, so the mosaic is
    logically padded by a transparent tile-strip on any edge whose tile index
    has the wrong parity.  Rather than materialise that padded array (a full
    copy of a mosaic that is tens of GB at base zoom), the pad is folded into
    the output: the real data is downsampled directly and placed at its
    tile-aligned offset inside a pre-zeroed coarse array.  Mosaic dimensions
    are always whole tiles (multiples of TILE_SIZE, hence even), so 2×2 blocks
    never straddle the real/pad boundary and the offset is exact.
    """
    pad_left = TILE_SIZE if tile_xmin % 2 != 0 else 0
    pad_right = TILE_SIZE if tile_xmax % 2 != 1 else 0
    pad_top = TILE_SIZE if tile_ymin % 2 != 0 else 0
    pad_bottom = TILE_SIZE if tile_ymax % 2 != 1 else 0

    # Padded tile bounds — even on every edge, so halving lands on the grid.
    p_xmin = tile_xmin - (1 if pad_left else 0)
    p_xmax = tile_xmax + (1 if pad_right else 0)
    p_ymin = tile_ymin - (1 if pad_top else 0)
    p_ymax = tile_ymax + (1 if pad_bottom else 0)

    c_xmin, c_xmax = p_xmin // 2, p_xmax // 2
    c_ymin, c_ymax = p_ymin // 2, p_ymax // 2
    coarse_h = (c_ymax - c_ymin + 1) * TILE_SIZE
    coarse_w = (c_xmax - c_xmin + 1) * TILE_SIZE

    reduced = _byteor_downsample(data)
    rh, rw, c = reduced.shape
    coarse = np.zeros((coarse_h, coarse_w, c), dtype=data.dtype)
    row_off = pad_top // 2
    col_off = pad_left // 2
    coarse[row_off : row_off + rh, col_off : col_off + rw, :] = reduced
    return coarse, c_xmin, c_xmax, c_ymin, c_ymax


def build_pyramid(
    base: BaseMosaic,
    min_zoom: int,
    output_dir: Path,
    compress_level: int = 6,
) -> None:
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
        _write_zoom_level_tiles(
            data, z, xmin, xmax, ymin, ymax, output_dir, compress_level
        )
        if z == min_zoom:
            break
        data, xmin, xmax, ymin, ymax = _downsample_level(
            data, xmin, xmax, ymin, ymax
        )
