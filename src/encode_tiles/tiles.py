"""Write the bitmask pyramid as XYZ-scheme gzip-compressed .bin tiles (ADR-0013).

The whole pyramid is built in one streaming pass: base-zoom tile-row strips
arrive north-to-south, each level writes its own tiles row by row and byte-OR
downsamples 2×2 into a single buffered parent tile-row that flushes to the
next-coarser level as soon as it completes.  Peak memory is one tile-row per
level — no zoom level's full mosaic is ever materialised.
"""

from __future__ import annotations

import gzip
from pathlib import Path
from typing import Iterable

import numpy as np

from encode_tiles.mosaic import TILE_SIZE, MosaicSpec

HALF_TILE = TILE_SIZE // 2


def _byteor_downsample(data: np.ndarray) -> np.ndarray:
    """Halve a multi-channel uint8 raster via byte-wise OR of 2×2 blocks."""
    h, w, c = data.shape
    b = data.reshape(h // 2, 2, w // 2, 2, c)
    return b[:, 0, :, 0, :] | b[:, 0, :, 1, :] | b[:, 1, :, 0, :] | b[:, 1, :, 1, :]


class _LevelWriter:
    """Writes one zoom level's tiles from top-down tile-row strips.

    Coarser 2×2 parent groups must align to the tile grid, so a child level
    whose min tile index is odd lands at a half-tile offset inside the parent
    row buffer; the pre-zeroed buffer supplies the transparent padding, and
    OR-ing zeros never flips a bit.  The buffer flushes to the parent writer
    once both child halves — or, on an odd southern edge, the final child
    row — have arrived, so exactly one tile-row buffer is resident per level.
    """

    def __init__(
        self,
        zoom: int,
        tile_xmin: int,
        tile_xmax: int,
        tile_ymin: int,
        tile_ymax: int,
        min_zoom: int,
        output_dir: Path,
        compress_level: int,
    ) -> None:
        self.zoom = zoom
        self.tile_xmin = tile_xmin
        self.tile_xmax = tile_xmax
        self.width = (tile_xmax - tile_xmin + 1) * TILE_SIZE
        self.output_dir = output_dir
        self.compress_level = compress_level
        self._buf: np.ndarray | None = None
        self._buf_ty = -1
        if zoom > min_zoom:
            self.parent: _LevelWriter | None = _LevelWriter(
                zoom - 1,
                tile_xmin // 2,
                tile_xmax // 2,
                tile_ymin // 2,
                tile_ymax // 2,
                min_zoom,
                output_dir,
                compress_level,
            )
            self._parent_col_off = (tile_xmin % 2) * HALF_TILE
        else:
            self.parent = None

    def write_tile_row(self, ty: int, strip: np.ndarray) -> None:
        """Write tile row `ty` from a (TILE_SIZE, width, bpp) strip and feed
        its downsample into the buffered parent row."""
        self._write_row_tiles(ty, strip)
        if self.parent is None:
            return

        reduced = _byteor_downsample(strip)
        if self._buf is None:
            self._buf = np.zeros(
                (TILE_SIZE, self.parent.width, strip.shape[2]), dtype=np.uint8
            )
            self._buf_ty = ty // 2
        row_off = (ty % 2) * HALF_TILE
        col_off = self._parent_col_off
        self._buf[row_off : row_off + HALF_TILE, col_off : col_off + reduced.shape[1], :] = reduced
        if ty % 2 == 1:
            self._flush_parent_row()

    def close(self) -> None:
        """Flush a half-filled parent row (odd southern edge) and cascade."""
        if self.parent is not None:
            if self._buf is not None:
                self._flush_parent_row()
            self.parent.close()

    def _flush_parent_row(self) -> None:
        assert self.parent is not None and self._buf is not None
        buf, self._buf = self._buf, None
        self.parent.write_tile_row(self._buf_ty, buf)

    def _write_row_tiles(self, ty: int, strip: np.ndarray) -> None:
        """Split one tile row into gzip .bin tiles; all-zero tiles are skipped."""
        for tx in range(self.tile_xmin, self.tile_xmax + 1):
            col0 = (tx - self.tile_xmin) * TILE_SIZE
            tile = strip[:, col0 : col0 + TILE_SIZE, :]
            if not np.any(tile):
                continue
            tile_dir = self.output_dir / str(self.zoom) / str(tx)
            tile_dir.mkdir(parents=True, exist_ok=True)
            with gzip.open(
                tile_dir / f"{ty}.bin", "wb", compresslevel=self.compress_level
            ) as f:
                f.write(tile.tobytes())


def build_pyramid(
    tile_rows: Iterable[tuple[int, np.ndarray]],
    spec: MosaicSpec,
    min_zoom: int,
    output_dir: Path,
    compress_level: int = 6,
) -> None:
    """Stream base-zoom tile-row strips into tiles at every zoom down to
    min_zoom, downsampling 2×2 byte-OR between levels as rows complete."""
    if min_zoom > spec.zoom:
        raise ValueError(
            f"min_zoom ({min_zoom}) must be <= base zoom ({spec.zoom})"
        )

    writer = _LevelWriter(
        spec.zoom,
        spec.tile_xmin,
        spec.tile_xmax,
        spec.tile_ymin,
        spec.tile_ymax,
        min_zoom,
        output_dir,
        compress_level,
    )
    for ty, strip in tile_rows:
        writer.write_tile_row(ty, strip)
    writer.close()
