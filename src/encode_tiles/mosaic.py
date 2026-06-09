"""Build the base-zoom mosaic from the packed bitmask GeoTIFF (ADR-0013).

The source is read in row-strips and reprojected (nearest-neighbour,
EPSG:4326 → EPSG:3857) directly into the corresponding row-strip of a
pre-allocated base mosaic.  Bytes are carried verbatim — the encoder never
interprets individual visibility bits.
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


def encode_source_to_base_raster(input_path: Path, zoom: int) -> BaseMosaic:
    """Read the packed bitmask GeoTIFF and reproject to EPSG:3857 at base zoom.

    Each source band is one byte of the per-pixel bitmask.  Bytes are carried
    verbatim using nearest-neighbour resampling; no encoding is applied.
    Azimuth metadata is read from the GeoTIFF tags written by the engine.
    """
    with rasterio.open(input_path) as src:
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
