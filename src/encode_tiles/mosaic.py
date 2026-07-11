"""Build the base-zoom mosaic strips from packed bitmask GeoTIFFs (ADR-0013).

Multiple input boxes are merged on one common EPSG:4326 lattice with
overlaps resolved by deepest-interior wins (ADR-0015).  Everything is
streamed in row-strips so peak memory is bounded by one strip, never a
whole box or a whole mosaic: the merge windows its sources straight from
disk and writes a temporary compressed GeoTIFF, and the 4326 → 3857
base-zoom reproject yields one tile-row strip at a time.  Bytes are carried
verbatim — the encoder never interprets individual visibility bits.
"""

from __future__ import annotations

import math
import tempfile
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence

import numpy as np
import rasterio
from rasterio.transform import Affine, from_bounds, from_origin
from rasterio.warp import Resampling, reproject, transform_bounds
from rasterio.windows import Window
from rasterio.windows import bounds as window_bounds
from rasterio.windows import from_bounds as window_from_bounds

WEB_MERCATOR_EXTENT = 20037508.342789244
TILE_SIZE = 512
# Rows per merged-lattice strip; also the merged GeoTIFF's block height so
# every strip write lands on whole compressed blocks exactly once.
MERGE_STRIP_HEIGHT = 512
# Extra source pixels read around each reprojected strip so nearest-neighbour
# lookups at strip edges never fall outside the window.
SRC_WINDOW_MARGIN = 2


@dataclass(frozen=True)
class MosaicSpec:
    """The base-zoom tile grid and format metadata — geometry only, no pixels."""

    zoom: int
    tile_xmin: int
    tile_xmax: int  # inclusive
    tile_ymin: int
    tile_ymax: int  # inclusive
    data_bounds_4326: tuple[float, float, float, float]  # west, south, east, north
    azimuth_min_deg: float
    azimuth_max_deg: float
    azimuth_step_deg: float
    bit_count: int
    bytes_per_pixel: int
    format_version: int


@dataclass(frozen=True)
class FormatContract:
    """The wire-format contract every input of one merged tileset must share.

    Read from the GeoTIFF tags the engine writes (ADR-0013); the merged
    tilejson carries exactly one of these (ADR-0015).
    """

    azimuth_min_deg: float
    azimuth_max_deg: float
    azimuth_step_deg: float
    bit_count: int
    format_version: int
    bytes_per_pixel: int


def _tile_extent_3857(zoom: int) -> float:
    return (2.0 * WEB_MERCATOR_EXTENT) / (2 ** zoom)


def _tile_range_for_bounds_3857(
    bounds_3857: tuple[float, float, float, float], zoom: int
) -> tuple[int, int, int, int]:
    west, south, east, north = bounds_3857
    te = _tile_extent_3857(zoom)
    n = 2 ** zoom
    eps = te * 1e-9
    xmin = int(math.floor((west + WEB_MERCATOR_EXTENT) / te))
    xmax = int(math.floor((east + WEB_MERCATOR_EXTENT - eps) / te))
    ymin = int(math.floor((WEB_MERCATOR_EXTENT - north) / te))
    ymax = int(math.floor((WEB_MERCATOR_EXTENT - south - eps) / te))
    xmin = max(0, xmin)
    xmax = min(n - 1, xmax)
    ymin = max(0, ymin)
    ymax = min(n - 1, ymax)
    return xmin, xmax, ymin, ymax


def _base_raster_geometry(
    tile_xmin: int,
    tile_xmax: int,
    tile_ymin: int,
    tile_ymax: int,
    zoom: int,
) -> tuple[tuple[float, float, float, float], int, int, Affine]:
    te = _tile_extent_3857(zoom)
    west = -WEB_MERCATOR_EXTENT + tile_xmin * te
    east = -WEB_MERCATOR_EXTENT + (tile_xmax + 1) * te
    north = WEB_MERCATOR_EXTENT - tile_ymin * te
    south = WEB_MERCATOR_EXTENT - (tile_ymax + 1) * te
    width = (tile_xmax - tile_xmin + 1) * TILE_SIZE
    height = (tile_ymax - tile_ymin + 1) * TILE_SIZE
    transform = from_bounds(west, south, east, north, width, height)
    return (west, south, east, north), width, height, transform


def _read_format_contract(src: rasterio.DatasetReader) -> FormatContract:
    tags = src.tags()
    return FormatContract(
        azimuth_min_deg=float(tags["azimuth_min_deg"]),
        azimuth_max_deg=float(tags["azimuth_max_deg"]),
        azimuth_step_deg=float(tags["azimuth_step_deg"]),
        bit_count=int(tags["bit_count"]),
        format_version=int(tags["format_version"]),
        bytes_per_pixel=src.count,
    )


def _merge_window(
    merged: np.ndarray,
    best: np.ndarray,
    data: np.ndarray,
    src_height: int,
    src_width: int,
    src_row0: int,
    dst_row0: int,
    dst_col0: int,
) -> None:
    """Merge one source's row-window into a strip of the output lattice.

    `data` holds rows [src_row0, src_row0 + rh) of a (src_height, src_width)
    source box; `merged`/`best` are the lattice strip and its running
    interior-distance scores, with the window landing at (dst_row0, dst_col0).
    The distance ramps depend only on global source geometry, so merging
    strip-by-strip is byte-identical to merging the whole lattice at once.
    """
    rh = data.shape[0]
    rows = np.arange(src_row0, src_row0 + rh, dtype=np.float32) + 0.5
    dist_ns = np.minimum(rows, src_height - rows)  # nearest of north/south edge
    cols = np.arange(src_width, dtype=np.float32)
    dist_e = src_width - cols - 0.5  # east edge only; west (offshore) excluded
    dist = np.minimum(dist_ns[:, None], dist_e[None, :])

    best_sub = best[dst_row0 : dst_row0 + rh, dst_col0 : dst_col0 + src_width]
    wins = dist > best_sub  # strict: an exact tie keeps the earlier source
    best_sub[wins] = dist[wins]
    merged[dst_row0 : dst_row0 + rh, dst_col0 : dst_col0 + src_width][wins] = data[wins]


def merge_deepest_interior(
    sources: Sequence[np.ndarray],
    offsets: Sequence[tuple[int, int]],
    out_height: int,
    out_width: int,
) -> np.ndarray:
    """Merge bitmask boxes on a common lattice; deepest interior wins (ADR-0015).

    Each source is an (h, w, bytes_per_pixel) uint8 array placed at its
    (row, col) offset on the output lattice.  Per pixel, each covering source
    scores its interior distance — the pixel-centre distance in cells to the
    nearest north / south / east edge of that source's box.  The west
    (offshore) edge is excluded so near-coast land is never down-weighted.
    The distances are continuous linear ramps, so the crossover between two
    overlapping boxes is a clean midline rather than a staircase.

    The deepest source's bytes are copied verbatim — bit-packed bytes are
    never blended.  Exact ties break to the lowest source index.  Pixels
    covered by no source stay all-zero (nodata → transparent).
    """
    if len(sources) != len(offsets):
        raise ValueError("sources and offsets must be the same length")
    bytes_per_pixel = sources[0].shape[2]
    merged = np.zeros((out_height, out_width, bytes_per_pixel), dtype=np.uint8)
    best = np.full((out_height, out_width), -np.inf, dtype=np.float32)

    for data, (row_off, col_off) in zip(sources, offsets):
        h, w, bpp = data.shape
        if bpp != bytes_per_pixel:
            raise ValueError("all sources must share bytes_per_pixel")
        if row_off < 0 or col_off < 0 or row_off + h > out_height or col_off + w > out_width:
            raise ValueError("source placement exceeds the output lattice")
        _merge_window(merged, best, data, h, w, 0, row_off, col_off)

    return merged


def merge_sources(input_paths: Sequence[Path], dst_path: Path) -> FormatContract:
    """Paste all input boxes onto one common 4326 lattice (ADR-0015).

    The lattice is anchored at the union's north-west corner on the first
    input's cell size; each input snaps to it by ≤ ½ cell.  All inputs must
    share the format contract, CRS, and resolution — a mismatch raises
    ValueError so the CLI aborts nonzero rather than silently merging
    incompatible tilesets.

    The merge is streamed in MERGE_STRIP_HEIGHT-row strips: each source is
    windowed straight from disk and the merged raster is written strip by
    strip to `dst_path` as a compressed GeoTIFF, so neither a whole box nor
    the whole lattice is ever resident in memory.
    """
    with ExitStack() as stack:
        datasets = [stack.enter_context(rasterio.open(p)) for p in input_paths]
        contracts = [_read_format_contract(ds) for ds in datasets]
        first_ds = datasets[0]
        first_contract = contracts[0]
        for path, ds, contract in zip(input_paths[1:], datasets[1:], contracts[1:]):
            if contract != first_contract:
                raise ValueError(
                    "format contract mismatch: "
                    f"{path} has {contract} "
                    f"but {input_paths[0]} has {first_contract}"
                )
            if ds.crs != first_ds.crs:
                raise ValueError(
                    f"CRS mismatch: {path} is {ds.crs}, "
                    f"{input_paths[0]} is {first_ds.crs}"
                )
            if not math.isclose(ds.res[0], first_ds.res[0], rel_tol=1e-6) or not (
                math.isclose(ds.res[1], first_ds.res[1], rel_tol=1e-6)
            ):
                raise ValueError(
                    f"resolution mismatch: {path} is {ds.res}, "
                    f"{input_paths[0]} is {first_ds.res}"
                )

        res_x, res_y = first_ds.res
        lattice_west = min(ds.bounds.left for ds in datasets)
        lattice_north = max(ds.bounds.top for ds in datasets)

        offsets = []
        out_height = 0
        out_width = 0
        for ds in datasets:
            # Snap each box onto the lattice (≤ ½ cell by rounding; resolutions
            # were validated equal above, so the error cannot accumulate).
            row_off = int(round((lattice_north - ds.bounds.top) / res_y))
            col_off = int(round((ds.bounds.left - lattice_west) / res_x))
            offsets.append((row_off, col_off))
            out_height = max(out_height, row_off + ds.height)
            out_width = max(out_width, col_off + ds.width)

        bytes_per_pixel = first_contract.bytes_per_pixel
        transform = from_origin(lattice_west, lattice_north, res_x, res_y)
        dst = stack.enter_context(
            rasterio.open(
                dst_path,
                "w",
                driver="GTiff",
                width=out_width,
                height=out_height,
                count=bytes_per_pixel,
                dtype="uint8",
                crs=first_ds.crs,
                transform=transform,
                tiled=True,
                blockxsize=TILE_SIZE,
                blockysize=MERGE_STRIP_HEIGHT,
                # The merged raster is a throwaway intermediate: cheapest
                # deflate level (it re-reads faster too), skip all-zero
                # blocks entirely, and compress on every core.
                compress="deflate",
                zlevel=1,
                sparse_ok=True,
                num_threads="ALL_CPUS",
                bigtiff="if_safer",
            )
        )

        for r0 in range(0, out_height, MERGE_STRIP_HEIGHT):
            rh = min(MERGE_STRIP_HEIGHT, out_height - r0)
            merged = np.zeros((rh, out_width, bytes_per_pixel), dtype=np.uint8)
            best = np.full((rh, out_width), -np.inf, dtype=np.float32)
            for ds, (row_off, col_off) in zip(datasets, offsets):
                sr0 = max(0, r0 - row_off)
                sr1 = min(ds.height, r0 + rh - row_off)
                if sr1 <= sr0:
                    continue
                window = Window(0, sr0, ds.width, sr1 - sr0)
                data = (
                    ds.read(window=window, masked=False)
                    .astype(np.uint8, copy=False)
                    .transpose(1, 2, 0)
                )
                _merge_window(
                    merged, best, data, ds.height, ds.width, sr0, row_off + sr0 - r0, col_off
                )
            dst.write(merged.transpose(2, 0, 1), window=Window(0, r0, out_width, rh))

    return first_contract


@contextmanager
def open_mosaic_source(
    input_paths: Sequence[Path],
) -> Iterator[tuple[rasterio.DatasetReader, FormatContract]]:
    """Open the single dataset the base-zoom encode reads from.

    A single input is opened directly — exactly the pre-multi-box path.
    Multiple inputs are first stream-merged (deepest-interior wins, ADR-0015)
    into a temporary compressed GeoTIFF that is deleted on exit.

    GDAL_NUM_THREADS stays set for the whole encode so GeoTIFF deflate
    codecs (and the warper, which defaults to it) use every core.
    """
    with rasterio.Env(GDAL_NUM_THREADS="ALL_CPUS"):
        if len(input_paths) == 1:
            with rasterio.open(input_paths[0]) as src:
                yield src, _read_format_contract(src)
            return

        with tempfile.TemporaryDirectory(prefix="encode_tiles_") as tmp_dir:
            merged_path = Path(tmp_dir) / "merged.tif"
            contract = merge_sources(input_paths, merged_path)
            with rasterio.open(merged_path) as src:
                yield src, contract


def compute_base_spec(
    src: rasterio.DatasetReader, contract: FormatContract, zoom: int
) -> MosaicSpec:
    """The base-zoom 3857 tile grid covering one open bitmask dataset."""
    src_bounds = src.bounds
    data_bounds_4326 = (
        float(src_bounds.left),
        float(src_bounds.bottom),
        float(src_bounds.right),
        float(src_bounds.top),
    )
    bounds_3857 = transform_bounds(
        src.crs, "EPSG:3857", *data_bounds_4326, densify_pts=21
    )
    tile_xmin, tile_xmax, tile_ymin, tile_ymax = _tile_range_for_bounds_3857(
        bounds_3857, zoom
    )
    return MosaicSpec(
        zoom=zoom,
        tile_xmin=tile_xmin,
        tile_xmax=tile_xmax,
        tile_ymin=tile_ymin,
        tile_ymax=tile_ymax,
        data_bounds_4326=data_bounds_4326,
        azimuth_min_deg=contract.azimuth_min_deg,
        azimuth_max_deg=contract.azimuth_max_deg,
        azimuth_step_deg=contract.azimuth_step_deg,
        bit_count=contract.bit_count,
        bytes_per_pixel=contract.bytes_per_pixel,
        format_version=contract.format_version,
    )


def _clipped_source_window(
    src: rasterio.DatasetReader, bounds_3857: tuple[float, float, float, float]
) -> Window | None:
    """The source pixel window covering the 3857 bounds, padded and clipped.

    Returns None when the bounds fall entirely outside the source raster.
    """
    bounds_src = transform_bounds("EPSG:3857", src.crs, *bounds_3857, densify_pts=21)
    window = window_from_bounds(*bounds_src, transform=src.transform)
    r0 = max(0, int(math.floor(window.row_off)) - SRC_WINDOW_MARGIN)
    r1 = min(src.height, int(math.ceil(window.row_off + window.height)) + SRC_WINDOW_MARGIN)
    c0 = max(0, int(math.floor(window.col_off)) - SRC_WINDOW_MARGIN)
    c1 = min(src.width, int(math.ceil(window.col_off + window.width)) + SRC_WINDOW_MARGIN)
    if r1 <= r0 or c1 <= c0:
        return None
    return Window(c0, r0, c1 - c0, r1 - r0)


def iter_base_tile_rows(
    src: rasterio.DatasetReader, spec: MosaicSpec, num_threads: int = 1
) -> Iterator[tuple[int, np.ndarray]]:
    """Yield (tile_y, strip) for every base-zoom tile row, north to south.

    Each strip is a (bytes_per_pixel, TILE_SIZE, mosaic_width) uint8 array —
    band-major, matching the warp's native layout so no full-strip transpose
    copies happen downstream — in EPSG:3857, reprojected nearest-neighbour
    from just the source window that tile row needs; only one strip and one
    source window are ever resident.  Each source band is one byte of the
    per-pixel bitmask; bytes are carried verbatim, and pixels no source
    covers stay all-zero (transparent).  `num_threads` fans the warp kernel
    out across cores.
    """
    _, width, _, dst_transform = _base_raster_geometry(
        spec.tile_xmin, spec.tile_xmax, spec.tile_ymin, spec.tile_ymax, spec.zoom
    )
    band_indices = list(range(1, spec.bytes_per_pixel + 1))

    for ti, ty in enumerate(range(spec.tile_ymin, spec.tile_ymax + 1)):
        strip_window = Window(0, ti * TILE_SIZE, width, TILE_SIZE)
        strip_bounds_3857 = window_bounds(strip_window, dst_transform)
        strip = np.zeros((spec.bytes_per_pixel, TILE_SIZE, width), dtype=np.uint8)

        src_window = _clipped_source_window(src, strip_bounds_3857)
        if src_window is not None:
            src_data = src.read(band_indices, window=src_window, masked=False).astype(
                np.uint8, copy=False
            )
            reproject(
                source=src_data,
                destination=strip,
                src_transform=src.window_transform(src_window),
                src_crs=src.crs,
                src_nodata=None,
                dst_transform=dst_transform * Affine.translation(0, ti * TILE_SIZE),
                dst_crs="EPSG:3857",
                dst_nodata=None,
                resampling=Resampling.nearest,
                num_threads=num_threads,
            )

        yield ty, strip
