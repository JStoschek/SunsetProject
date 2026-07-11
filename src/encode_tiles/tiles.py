"""Write the bitmask pyramid as XYZ-scheme gzip-compressed .bin tiles (ADR-0013).

The whole pyramid is built in one streaming pass: base-zoom tile-row strips
arrive north-to-south in band-major (bytes_per_pixel, H, W) layout, each
level writes its own tiles row by row and byte-OR downsamples 2×2 into a
single buffered parent tile-row that flushes to the next-coarser level as
soon as it completes.  Peak memory is one tile-row per level — no zoom
level's full mosaic is ever materialised.

Tile compression is fanned out to a thread pool: zlib releases the GIL, so
gzipping one row's tiles overlaps the warp and downsample of the next.
"""

from __future__ import annotations

import gzip
import os
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Iterable

import numpy as np

from encode_tiles.mosaic import TILE_SIZE, MosaicSpec

HALF_TILE = TILE_SIZE // 2
# Cap on tiles queued for compression.  A pending tile pins the strip it
# views, so an unbounded queue would quietly re-grow whole-mosaic memory;
# 32 tiles keep every worker busy while pinning at most a strip or two.
MAX_PENDING_TILES = 32


def _byteor_downsample(data: np.ndarray) -> np.ndarray:
    """Halve a (channels, H, W) uint8 raster via byte-wise OR of 2×2 blocks."""
    c, h, w = data.shape
    b = data.reshape(c, h // 2, 2, w // 2, 2)
    out = b[:, :, 0, :, 0] | b[:, :, 0, :, 1]
    out |= b[:, :, 1, :, 0]
    out |= b[:, :, 1, :, 1]
    return out


class _TileSink:
    """Compresses and writes tiles on a thread pool, bounded by MAX_PENDING_TILES.

    Workers only touch numpy views, gzip, and the filesystem — never GDAL —
    so the main thread's warp and the pool never contend on rasterio state.
    """

    def __init__(self, output_dir: Path, compress_level: int, workers: int) -> None:
        self._output_dir = output_dir
        self._compress_level = compress_level
        self._executor = ThreadPoolExecutor(max_workers=workers)
        self._slots = threading.BoundedSemaphore(MAX_PENDING_TILES)
        self._futures: list[Future[None]] = []

    def submit(self, zoom: int, tx: int, ty: int, tile: np.ndarray) -> None:
        """Queue one (bpp, TILE_SIZE, TILE_SIZE) tile view for compression."""
        self._slots.acquire()
        self._futures.append(
            self._executor.submit(self._write_one, zoom, tx, ty, tile)
        )

    def _write_one(self, zoom: int, tx: int, ty: int, tile: np.ndarray) -> None:
        try:
            # (bpp, H, W) → interleaved (H, W, bpp) bytes per ADR-0013;
            # mtime=0 keeps re-encodes byte-identical.
            raw = tile.transpose(1, 2, 0).tobytes()
            payload = gzip.compress(raw, compresslevel=self._compress_level, mtime=0)
            tile_dir = self._output_dir / str(zoom) / str(tx)
            tile_dir.mkdir(parents=True, exist_ok=True)
            (tile_dir / f"{ty}.bin").write_bytes(payload)
        finally:
            self._slots.release()

    def finish(self) -> None:
        """Drain the queue and re-raise the first worker failure, if any."""
        self._executor.shutdown(wait=True)
        for future in self._futures:
            future.result()

    def abort(self) -> None:
        self._executor.shutdown(wait=False, cancel_futures=True)


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
        sink: _TileSink,
    ) -> None:
        self.zoom = zoom
        self.tile_xmin = tile_xmin
        self.tile_xmax = tile_xmax
        self.width = (tile_xmax - tile_xmin + 1) * TILE_SIZE
        self._sink = sink
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
                sink,
            )
            self._parent_col_off = (tile_xmin % 2) * HALF_TILE
        else:
            self.parent = None

    def write_tile_row(self, ty: int, strip: np.ndarray) -> None:
        """Write tile row `ty` from a (bpp, TILE_SIZE, width) strip and feed
        its downsample into the buffered parent row."""
        self._write_row_tiles(ty, strip)
        if self.parent is None:
            return

        reduced = _byteor_downsample(strip)
        if self._buf is None:
            self._buf = np.zeros(
                (strip.shape[0], TILE_SIZE, self.parent.width), dtype=np.uint8
            )
            self._buf_ty = ty // 2
        row_off = (ty % 2) * HALF_TILE
        col_off = self._parent_col_off
        self._buf[:, row_off : row_off + HALF_TILE, col_off : col_off + reduced.shape[2]] = reduced
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
        """Queue one tile row's non-empty tiles; all-zero tiles are skipped.

        Occupancy comes from a single fused reduction over the contiguous
        strip rather than one np.any per tile.
        """
        bpp = strip.shape[0]
        n_tiles = self.width // TILE_SIZE
        occupied = strip.reshape(bpp, TILE_SIZE, n_tiles, TILE_SIZE).any(axis=(0, 1, 3))
        for i in np.flatnonzero(occupied):
            col0 = int(i) * TILE_SIZE
            self._sink.submit(
                self.zoom,
                self.tile_xmin + int(i),
                ty,
                strip[:, :, col0 : col0 + TILE_SIZE],
            )


def build_pyramid(
    tile_rows: Iterable[tuple[int, np.ndarray]],
    spec: MosaicSpec,
    min_zoom: int,
    output_dir: Path,
    compress_level: int = 6,
    workers: int | None = None,
) -> None:
    """Stream base-zoom tile-row strips into tiles at every zoom down to
    min_zoom, downsampling 2×2 byte-OR between levels as rows complete."""
    if min_zoom > spec.zoom:
        raise ValueError(
            f"min_zoom ({min_zoom}) must be <= base zoom ({spec.zoom})"
        )

    sink = _TileSink(output_dir, compress_level, workers or os.cpu_count() or 4)
    try:
        writer = _LevelWriter(
            spec.zoom,
            spec.tile_xmin,
            spec.tile_xmax,
            spec.tile_ymin,
            spec.tile_ymax,
            min_zoom,
            sink,
        )
        for ty, strip in tile_rows:
            writer.write_tile_row(ty, strip)
        writer.close()
    except BaseException:
        sink.abort()
        raise
    sink.finish()
