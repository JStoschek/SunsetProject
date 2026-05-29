"""Write the RGBA pyramid as XYZ-scheme PNG tiles."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

from encode_tiles.mosaic import TILE_SIZE, BaseMosaic


def _write_zoom_level_tiles(
    rgba: np.ndarray,
    zoom: int,
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
    output_dir: Path,
) -> int:
    """Split an RGBA mosaic into 512x512 PNGs. Returns count written.

    Tiles whose alpha is entirely zero are skipped.
    """
    count = 0
    for ty in range(tile_ymin, tile_ymax + 1):
        row0 = (ty - tile_ymin) * TILE_SIZE
        row1 = row0 + TILE_SIZE
        for tx in range(tile_xmin, tile_xmax + 1):
            col0 = (tx - tile_xmin) * TILE_SIZE
            col1 = col0 + TILE_SIZE
            tile = rgba[row0:row1, col0:col1, :]
            if not np.any(tile[..., 3]):
                continue
            tile_dir = output_dir / str(zoom) / str(tx)
            tile_dir.mkdir(parents=True, exist_ok=True)
            Image.fromarray(tile, mode="RGBA").save(
                tile_dir / f"{ty}.png", format="PNG", optimize=False
            )
            count += 1
    return count


def _alpha_weighted_downsample(rgba: np.ndarray) -> np.ndarray:
    """Halve an RGBA raster via alpha-weighted 2x2 averaging.

    Input height and width must be even. Output dtype is uint8.
    Transparent pixels (A=0) contribute nothing; if all four are transparent
    the output is (0,0,0,0). Otherwise A=255 and R/G are the alpha-weighted
    mean of the opaque inputs.
    """
    h, w, _ = rgba.shape
    blocks = rgba.reshape(h // 2, 2, w // 2, 2, 4).astype(np.uint32)
    r = blocks[..., 0]
    g = blocks[..., 1]
    a = blocks[..., 3]
    total_a = a.sum(axis=(1, 3))
    out = np.zeros((h // 2, w // 2, 4), dtype=np.uint8)
    mask = total_a > 0
    if mask.any():
        aw_r = (r * a).sum(axis=(1, 3))
        aw_g = (g * a).sum(axis=(1, 3))
        out[..., 0][mask] = np.round(aw_r[mask] / total_a[mask]).astype(np.uint8)
        out[..., 1][mask] = np.round(aw_g[mask] / total_a[mask]).astype(np.uint8)
        out[..., 3][mask] = 255
    return out


def _pad_for_downsample(
    rgba: np.ndarray,
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
) -> tuple[np.ndarray, int, int, int, int]:
    """Pad the mosaic with transparent tile-strips so that 2x2 parent groups
    at the next-coarser zoom align with mosaic edges.

    Each parent tile spans children (2k, 2k+1); the mosaic must therefore
    begin on an even child index and end on an odd one along each axis.
    """
    pad_left = TILE_SIZE if tile_xmin % 2 != 0 else 0
    pad_right = TILE_SIZE if tile_xmax % 2 != 1 else 0
    pad_top = TILE_SIZE if tile_ymin % 2 != 0 else 0
    pad_bottom = TILE_SIZE if tile_ymax % 2 != 1 else 0

    if pad_left or pad_right or pad_top or pad_bottom:
        rgba = np.pad(
            rgba,
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
    return rgba, tile_xmin, tile_xmax, tile_ymin, tile_ymax


def build_pyramid(base: BaseMosaic, min_zoom: int, output_dir: Path) -> None:
    """Walk zooms top-down from base.zoom to min_zoom, writing tiles and
    downsampling 2x2 between levels. Only one level's mosaic is resident.
    """
    if min_zoom > base.zoom:
        raise ValueError(
            f"min_zoom ({min_zoom}) must be <= base zoom ({base.zoom})"
        )

    rgba = base.rgba
    xmin, xmax = base.tile_xmin, base.tile_xmax
    ymin, ymax = base.tile_ymin, base.tile_ymax

    for z in range(base.zoom, min_zoom - 1, -1):
        _write_zoom_level_tiles(rgba, z, xmin, xmax, ymin, ymax, output_dir)
        if z == min_zoom:
            break
        rgba, xmin, xmax, ymin, ymax = _pad_for_downsample(
            rgba, xmin, xmax, ymin, ymax
        )
        rgba = _alpha_weighted_downsample(rgba)
        xmin, xmax = xmin // 2, xmax // 2
        ymin, ymax = ymin // 2, ymax // 2
