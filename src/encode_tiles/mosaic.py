"""Build the base-zoom RGBA mosaic from the two-band source GeoTIFF.

The source is read in row-strips and reprojected (nearest-neighbour,
EPSG:4326 -> EPSG:3857) directly into the corresponding row-strip of a
pre-allocated base mosaic. The full source raster is never simultaneously
resident with the full base RGBA mosaic.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import rasterio
from rasterio.transform import Affine, from_bounds
from rasterio.warp import Resampling, reproject, transform_bounds
from rasterio.windows import Window
from rasterio.windows import bounds as window_bounds

from encode_tiles.encoder import AZ_OFFSET, AZ_RANGE

WEB_MERCATOR_EXTENT = 20037508.342789244
TILE_SIZE = 512
SRC_STRIP_HEIGHT = 256


@dataclass(frozen=True)
class BaseMosaic:
    """The base-zoom RGBA mosaic and the tile grid it covers."""

    rgba: np.ndarray  # (H, W, 4) uint8
    zoom: int
    tile_xmin: int
    tile_xmax: int  # inclusive
    tile_ymin: int
    tile_ymax: int  # inclusive
    data_bounds_4326: tuple[float, float, float, float]  # west, south, east, north


def _tile_extent_3857(zoom: int) -> float:
    return (2.0 * WEB_MERCATOR_EXTENT) / (2 ** zoom)


def _tile_range_for_bounds_3857(
    bounds_3857: tuple[float, float, float, float], zoom: int
) -> tuple[int, int, int, int]:
    west, south, east, north = bounds_3857
    te = _tile_extent_3857(zoom)
    n = 2 ** zoom
    # Tiny epsilon to avoid grabbing a neighbouring tile when the bound lands
    # exactly on a tile edge.
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


def _validate_source_strip(
    min_az: np.ndarray, max_az: np.ndarray, row_offset: int
) -> None:
    nan_min = np.isnan(min_az)
    nan_max = np.isnan(max_az)
    one_nan = nan_min ^ nan_max
    if one_nan.any():
        ys, xs = np.where(one_nan)
        y = int(ys[0]) + row_offset
        x = int(xs[0])
        raise ValueError(
            f"source pixel (x={x}, y={y}): exactly one of min_az/max_az is NaN "
            f"(min_az={float(min_az[ys[0], xs[0]])}, max_az={float(max_az[ys[0], xs[0]])})"
        )
    valid = ~(nan_min | nan_max)
    if not valid.any():
        return
    # (+inf, -inf) sentinel = land cell with empty visible-azimuth range (ADR-0003).
    sentinel = valid & np.isposinf(min_az) & np.isneginf(max_az)
    bad = valid & ~sentinel & (
        (min_az < AZ_OFFSET)
        | (max_az > AZ_OFFSET + AZ_RANGE)
        | (min_az > max_az)
    )
    if bad.any():
        ys, xs = np.where(bad)
        y = int(ys[0]) + row_offset
        x = int(xs[0])
        raise ValueError(
            f"source pixel (x={x}, y={y}): min/max azimuth out of range "
            f"[{AZ_OFFSET}, {AZ_OFFSET + AZ_RANGE}] or min > max "
            f"(min_az={float(min_az[ys[0], xs[0]])}, max_az={float(max_az[ys[0], xs[0]])})"
        )


def _encode_array(min_az: np.ndarray, max_az: np.ndarray) -> np.ndarray:
    """Vectorised encode_pixel. NaN→(0,0,0,0), (+inf,-inf)→(255,0,0,255). Assumes prior validation."""
    h, w = min_az.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    not_nan = ~(np.isnan(min_az) | np.isnan(max_az))
    sentinel = not_nan & np.isposinf(min_az) & np.isneginf(max_az)
    valid = not_nan & ~sentinel
    if sentinel.any():
        rgba[..., 0][sentinel] = 255
        rgba[..., 3][sentinel] = 255
    if valid.any():
        mn = min_az[valid]
        mx = max_az[valid]
        r = np.round((mn - AZ_OFFSET) / AZ_RANGE * 255).astype(np.uint8)
        g = np.round((mx - AZ_OFFSET) / AZ_RANGE * 255).astype(np.uint8)
        rgba[..., 0][valid] = r
        rgba[..., 1][valid] = g
        rgba[..., 3][valid] = 255
    return rgba


def encode_source_to_base_raster(input_path: Path, zoom: int) -> BaseMosaic:
    """Read the source GeoTIFF in row-strips, reproject to EPSG:3857 at the
    given base zoom, and encode each strip into a pre-allocated RGBA mosaic.
    """
    with rasterio.open(input_path) as src:
        if src.count < 2:
            raise ValueError(
                f"source raster must have at least 2 bands (min_az, max_az); "
                f"got {src.count}"
            )
        src_crs = src.crs
        src_bounds = src.bounds  # in src CRS, expected EPSG:4326
        data_bounds_4326 = (
            float(src_bounds.left),
            float(src_bounds.bottom),
            float(src_bounds.right),
            float(src_bounds.top),
        )
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

        base_rgba = np.zeros((height, width, 4), dtype=np.uint8)

        for r0 in range(0, src.height, SRC_STRIP_HEIGHT):
            rh = min(SRC_STRIP_HEIGHT, src.height - r0)
            window = Window(0, r0, src.width, rh)
            src_strip = src.read(
                [1, 2], window=window, masked=False
            ).astype(np.float32, copy=False)
            # Mark explicit nodata as NaN if the source declares one.
            if src.nodata is not None and not math.isnan(src.nodata):
                src_strip = np.where(src_strip == src.nodata, np.nan, src_strip)

            _validate_source_strip(src_strip[0], src_strip[1], r0)

            src_strip_transform = src.window_transform(window)
            strip_bounds_src = window_bounds(window, src.transform)
            strip_bounds_3857 = transform_bounds(
                src_crs, "EPSG:3857", *strip_bounds_src, densify_pts=21
            )
            # Determine the destination row range this source strip covers.
            base_res_y = (raster_bounds[3] - raster_bounds[1]) / height
            dy_top = int(math.floor((dst_north - strip_bounds_3857[3]) / base_res_y))
            dy_bot = int(math.ceil((dst_north - strip_bounds_3857[1]) / base_res_y))
            dy_top = max(0, dy_top)
            dy_bot = min(height, dy_bot)
            if dy_bot <= dy_top:
                continue

            dst_strip = np.full(
                (2, dy_bot - dy_top, width), np.nan, dtype=np.float32
            )
            dst_strip_transform = dst_transform * Affine.translation(0, dy_top)
            reproject(
                source=src_strip,
                destination=dst_strip,
                src_transform=src_strip_transform,
                src_crs=src_crs,
                src_nodata=np.nan,
                dst_transform=dst_strip_transform,
                dst_crs="EPSG:3857",
                dst_nodata=np.nan,
                resampling=Resampling.nearest,
            )
            base_rgba[dy_top:dy_bot, :, :] = _encode_array(
                dst_strip[0], dst_strip[1]
            )

    return BaseMosaic(
        rgba=base_rgba,
        zoom=zoom,
        tile_xmin=tile_xmin,
        tile_xmax=tile_xmax,
        tile_ymin=tile_ymin,
        tile_ymax=tile_ymax,
        data_bounds_4326=data_bounds_4326,
    )
