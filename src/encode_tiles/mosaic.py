"""Build the base-zoom mosaic from packed bitmask GeoTIFFs (ADR-0013).

Multiple input boxes are first merged on one common EPSG:4326 lattice with
overlaps resolved by deepest-interior wins (ADR-0015), then the single merged
raster is read in row-strips and reprojected (nearest-neighbour,
EPSG:4326 → EPSG:3857) directly into the corresponding row-strip of a
pre-allocated base mosaic.  Bytes are carried verbatim — the encoder never
interprets individual visibility bits.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np
import rasterio
from rasterio.io import MemoryFile
from rasterio.transform import from_bounds, from_origin
from rasterio.transform import Affine
from rasterio.warp import Resampling, reproject, transform_bounds
from rasterio.windows import Window
from rasterio.windows import bounds as window_bounds

WEB_MERCATOR_EXTENT = 20037508.342789244
TILE_SIZE = 512
SRC_STRIP_HEIGHT = 256


@dataclass(frozen=True)
class BaseMosaic:
    """The base-zoom bitmask mosaic and the tile grid it covers."""

    data: np.ndarray  # (H, W, bytes_per_pixel) uint8
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

        rows = np.arange(h, dtype=np.float32) + 0.5  # pixel-centre distance to north edge
        cols = np.arange(w, dtype=np.float32)
        dist_ns = np.minimum(rows, h - rows)  # nearest of north/south edge
        dist_e = w - cols - 0.5  # east edge only; west (offshore) excluded
        dist = np.minimum(dist_ns[:, None], dist_e[None, :])

        best_sub = best[row_off : row_off + h, col_off : col_off + w]
        wins = dist > best_sub  # strict: an exact tie keeps the earlier source
        best_sub[wins] = dist[wins]
        merged[row_off : row_off + h, col_off : col_off + w][wins] = data[wins]

    return merged


def merge_sources(
    input_paths: Sequence[Path],
) -> tuple[np.ndarray, Affine, FormatContract]:
    """Paste all input boxes onto one common 4326 lattice (ADR-0015).

    The lattice is anchored at the union's north-west corner on the first
    input's cell size; each input snaps to it by ≤ ½ cell.  All inputs must
    share the format contract, CRS, and resolution — a mismatch raises
    ValueError so the CLI aborts nonzero rather than silently merging
    incompatible tilesets.
    """
    metas = []
    for path in input_paths:
        with rasterio.open(path) as src:
            bands = list(range(1, src.count + 1))
            data = src.read(bands, masked=False).astype(np.uint8, copy=False)
            metas.append(
                {
                    "path": path,
                    "contract": _read_format_contract(src),
                    "crs": src.crs,
                    "res": src.res,
                    "bounds": src.bounds,
                    "data": data.transpose(1, 2, 0),  # (h, w, bytes_per_pixel)
                }
            )

    first = metas[0]
    for m in metas[1:]:
        if m["contract"] != first["contract"]:
            raise ValueError(
                "format contract mismatch: "
                f"{m['path']} has {m['contract']} "
                f"but {first['path']} has {first['contract']}"
            )
        if m["crs"] != first["crs"]:
            raise ValueError(
                f"CRS mismatch: {m['path']} is {m['crs']}, "
                f"{first['path']} is {first['crs']}"
            )
        if not math.isclose(m["res"][0], first["res"][0], rel_tol=1e-6) or not (
            math.isclose(m["res"][1], first["res"][1], rel_tol=1e-6)
        ):
            raise ValueError(
                f"resolution mismatch: {m['path']} is {m['res']}, "
                f"{first['path']} is {first['res']}"
            )

    res_x, res_y = first["res"]
    lattice_west = min(m["bounds"].left for m in metas)
    lattice_north = max(m["bounds"].top for m in metas)

    sources = []
    offsets = []
    out_height = 0
    out_width = 0
    for m in metas:
        # Snap each box onto the lattice (≤ ½ cell by rounding; resolutions
        # were validated equal above, so the error cannot accumulate).
        row_off = int(round((lattice_north - m["bounds"].top) / res_y))
        col_off = int(round((m["bounds"].left - lattice_west) / res_x))
        h, w, _ = m["data"].shape
        sources.append(m["data"])
        offsets.append((row_off, col_off))
        out_height = max(out_height, row_off + h)
        out_width = max(out_width, col_off + w)

    merged = merge_deepest_interior(sources, offsets, out_height, out_width)
    transform = from_origin(lattice_west, lattice_north, res_x, res_y)
    return merged, transform, first["contract"]


def encode_sources_to_base_raster(
    input_paths: Sequence[Path], zoom: int
) -> BaseMosaic:
    """Encode one or more bitmask GeoTIFFs into the base-zoom 3857 mosaic.

    A single input takes exactly the pre-multi-box path.  Multiple inputs are
    merged on a common 4326 lattice first (deepest-interior wins, ADR-0015),
    then the one merged raster goes through the same unchanged 4326 → 3857
    reproject-and-tile pass.
    """
    if len(input_paths) == 1:
        return encode_source_to_base_raster(input_paths[0], zoom)

    merged, transform, contract = merge_sources(input_paths)
    height, width, bytes_per_pixel = merged.shape
    with MemoryFile() as memfile:
        with memfile.open(
            driver="GTiff",
            width=width,
            height=height,
            count=bytes_per_pixel,
            dtype="uint8",
            crs="EPSG:4326",
            transform=transform,
        ) as dst:
            dst.write(merged.transpose(2, 0, 1))
            dst.update_tags(
                azimuth_min_deg=str(contract.azimuth_min_deg),
                azimuth_max_deg=str(contract.azimuth_max_deg),
                azimuth_step_deg=str(contract.azimuth_step_deg),
                bit_count=str(contract.bit_count),
                format_version=str(contract.format_version),
            )
        with memfile.open() as src:
            return _encode_open_source(src, zoom)


def encode_source_to_base_raster(input_path: Path, zoom: int) -> BaseMosaic:
    """Read the packed bitmask GeoTIFF and reproject to EPSG:3857 at base zoom.

    Each source band is one byte of the per-pixel bitmask.  Bytes are carried
    verbatim using nearest-neighbour resampling; no encoding is applied.
    Azimuth metadata is read from the GeoTIFF tags written by the engine.
    """
    with rasterio.open(input_path) as src:
        return _encode_open_source(src, zoom)


def _encode_open_source(src: rasterio.DatasetReader, zoom: int) -> BaseMosaic:
    """Reproject one open bitmask dataset to EPSG:3857 at base zoom."""
    bytes_per_pixel = src.count
    src_crs = src.crs
    src_bounds = src.bounds
    data_bounds_4326 = (
        float(src_bounds.left),
        float(src_bounds.bottom),
        float(src_bounds.right),
        float(src_bounds.top),
    )

    tags = src.tags()
    azimuth_min_deg = float(tags["azimuth_min_deg"])
    azimuth_max_deg = float(tags["azimuth_max_deg"])
    azimuth_step_deg = float(tags["azimuth_step_deg"])
    bit_count = int(tags["bit_count"])
    format_version = int(tags["format_version"])

    bounds_3857 = transform_bounds(
        src_crs, "EPSG:3857", *data_bounds_4326, densify_pts=21
    )
    tile_xmin, tile_xmax, tile_ymin, tile_ymax = _tile_range_for_bounds_3857(
        bounds_3857, zoom
    )
    raster_bounds, width, height, dst_transform = _base_raster_geometry(
        tile_xmin, tile_xmax, tile_ymin, tile_ymax, zoom
    )
    dst_north = raster_bounds[3]

    base_data = np.zeros((height, width, bytes_per_pixel), dtype=np.uint8)
    band_indices = list(range(1, bytes_per_pixel + 1))

    for r0 in range(0, src.height, SRC_STRIP_HEIGHT):
        rh = min(SRC_STRIP_HEIGHT, src.height - r0)
        window = Window(0, r0, src.width, rh)
        src_strip = src.read(band_indices, window=window, masked=False).astype(
            np.uint8, copy=False
        )  # shape: (bytes_per_pixel, rh, src.width)

        src_strip_transform = src.window_transform(window)
        strip_bounds_src = window_bounds(window, src.transform)
        strip_bounds_3857 = transform_bounds(
            src_crs, "EPSG:3857", *strip_bounds_src, densify_pts=21
        )
        base_res_y = (raster_bounds[3] - raster_bounds[1]) / height
        dy_top = int(math.floor((dst_north - strip_bounds_3857[3]) / base_res_y))
        dy_bot = int(math.ceil((dst_north - strip_bounds_3857[1]) / base_res_y))
        dy_top = max(0, dy_top)
        dy_bot = min(height, dy_bot)
        if dy_bot <= dy_top:
            continue

        dst_strip = np.zeros(
            (bytes_per_pixel, dy_bot - dy_top, width), dtype=np.uint8
        )
        dst_strip_transform = dst_transform * Affine.translation(0, dy_top)
        reproject(
            source=src_strip,
            destination=dst_strip,
            src_transform=src_strip_transform,
            src_crs=src_crs,
            src_nodata=None,
            dst_transform=dst_strip_transform,
            dst_crs="EPSG:3857",
            dst_nodata=None,
            resampling=Resampling.nearest,
        )
        # transpose (bytes_per_pixel, H, W) → (H, W, bytes_per_pixel)
        base_data[dy_top:dy_bot, :, :] = dst_strip.transpose(1, 2, 0)

    return BaseMosaic(
        data=base_data,
        zoom=zoom,
        tile_xmin=tile_xmin,
        tile_xmax=tile_xmax,
        tile_ymin=tile_ymin,
        tile_ymax=tile_ymax,
        data_bounds_4326=data_bounds_4326,
        azimuth_min_deg=azimuth_min_deg,
        azimuth_max_deg=azimuth_max_deg,
        azimuth_step_deg=azimuth_step_deg,
        bit_count=bit_count,
        bytes_per_pixel=bytes_per_pixel,
        format_version=format_version,
    )
