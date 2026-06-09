"""End-to-end tests for the bitmask encoder pipeline (ADR-0013)."""

from __future__ import annotations

import gzip
import json
import math
from pathlib import Path

import numpy as np
import pytest
import rasterio
from rasterio.transform import from_bounds

from encode_tiles.cli import main
from encode_tiles.mosaic import TILE_SIZE, WEB_MERCATOR_EXTENT


# Production azimuth config (matches vectors.json).
BYTES_PER_PIXEL = 10
AZ_MIN = 233.0
AZ_MAX = 309.0
AZ_STEP = 1.0
BIT_COUNT = 77
FORMAT_VERSION = 1
# Flag bit: index=77 → byte 9, mask 0x20.
_FLAG_BYTE = 9
_FLAG_MASK = 0x20

SRC_HEIGHT = 4
SRC_WEST = 0.0
SRC_EAST = 4.0
SRC_SOUTH = 0.0
SRC_NORTH = 4.0
# col 0 = 0–1°E, row 0 = 3–4°N (northernmost)
LAND_ROW, LAND_COL = 0, 0
OCEAN_ROW, OCEAN_COL = 1, 1
ZOOM = 10


def _land_pixel() -> bytes:
    """10 bytes: bit 0 (visible at 233°) + flag bit."""
    b = bytearray(BYTES_PER_PIXEL)
    b[0] = 0x01
    b[_FLAG_BYTE] = _FLAG_MASK
    return bytes(b)


def _transparent_pixel() -> bytes:
    return bytes(BYTES_PER_PIXEL)


def _make_fixture(path: Path) -> None:
    """Write a 4×4 10-band uint8 GeoTIFF with production azimuth metadata."""
    land = _land_pixel()
    bands = []
    for band_idx in range(BYTES_PER_PIXEL):
        arr = np.zeros((SRC_HEIGHT, 4), dtype=np.uint8)
        arr[LAND_ROW, LAND_COL] = land[band_idx]
        bands.append(arr)

    transform = from_bounds(SRC_WEST, SRC_SOUTH, SRC_EAST, SRC_NORTH, 4, 4)
    with rasterio.open(
        path,
        "w",
        driver="GTiff",
        width=4,
        height=SRC_HEIGHT,
        count=BYTES_PER_PIXEL,
        dtype="uint8",
        crs="EPSG:4326",
        transform=transform,
    ) as dst:
        for i, arr in enumerate(bands, start=1):
            dst.write(arr, i)
        dst.update_tags(
            azimuth_min_deg=str(AZ_MIN),
            azimuth_max_deg=str(AZ_MAX),
            azimuth_step_deg=str(AZ_STEP),
            bit_count=str(BIT_COUNT),
            format_version=str(FORMAT_VERSION),
        )


def _lonlat_to_3857(lon: float, lat: float) -> tuple[float, float]:
    x = math.radians(lon) * 6378137.0
    y = math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)) * 6378137.0
    return x, y


def _world_to_tile(x: float, y: float, zoom: int) -> tuple[int, int, int, int]:
    """3857 coords → (tile_x, tile_y, pixel_x_in_tile, pixel_y_in_tile)."""
    tile_extent = (2.0 * WEB_MERCATOR_EXTENT) / (2 ** zoom)
    fx = (x + WEB_MERCATOR_EXTENT) / tile_extent
    fy = (WEB_MERCATOR_EXTENT - y) / tile_extent
    tx = int(math.floor(fx))
    ty = int(math.floor(fy))
    px = int(math.floor((fx - tx) * TILE_SIZE))
    py = int(math.floor((fy - ty) * TILE_SIZE))
    return tx, ty, px, py


def test_single_zoom_pipeline(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)

    rc = main(
        [
            "--input", str(src_tif),
            "--output-dir", str(out_dir),
            "--min-zoom", str(ZOOM),
            "--max-zoom", str(ZOOM),
        ]
    )
    assert rc == 0

    # --- TileJSON has correct format fields ---
    tj_path = out_dir / "tilejson.json"
    assert tj_path.is_file()
    tj = json.loads(tj_path.read_text())
    assert tj["format"] == "sunset-bitmask"
    assert tj["format_version"] == FORMAT_VERSION
    assert tj["azimuth_min_deg"] == pytest.approx(AZ_MIN)
    assert tj["azimuth_max_deg"] == pytest.approx(AZ_MAX)
    assert tj["azimuth_step_deg"] == pytest.approx(AZ_STEP)
    assert tj["bit_count"] == BIT_COUNT
    assert tj["bytes_per_pixel"] == BYTES_PER_PIXEL
    assert tj["tiles"] == ["{z}/{x}/{y}.bin"]
    assert tj["minzoom"] == ZOOM
    assert tj["maxzoom"] == ZOOM
    west, south, east, north = tj["bounds"]
    assert west == pytest.approx(SRC_WEST, abs=1e-6)
    assert south == pytest.approx(SRC_SOUTH, abs=1e-6)
    assert east == pytest.approx(SRC_EAST, abs=1e-6)
    assert north == pytest.approx(SRC_NORTH, abs=1e-6)

    # --- Known land pixel: bytes appear unaltered in .bin tile ---
    land_lon = SRC_WEST + LAND_COL + 0.5
    land_lat = SRC_NORTH - LAND_ROW - 0.5
    lx, ly = _lonlat_to_3857(land_lon, land_lat)
    tx, ty, px, py = _world_to_tile(lx, ly, ZOOM)
    tile_path = out_dir / str(ZOOM) / str(tx) / f"{ty}.bin"
    assert tile_path.is_file(), f"expected land tile at {tile_path}"

    with gzip.open(tile_path, "rb") as f:
        raw = f.read()
    assert len(raw) == TILE_SIZE * TILE_SIZE * BYTES_PER_PIXEL, (
        f"tile body is {len(raw)} bytes, expected {TILE_SIZE * TILE_SIZE * BYTES_PER_PIXEL}"
    )
    pixel_offset = (py * TILE_SIZE + px) * BYTES_PER_PIXEL
    pixel_bytes = bytes(raw[pixel_offset : pixel_offset + BYTES_PER_PIXEL])
    assert pixel_bytes == _land_pixel(), (
        f"pixel bytes {pixel_bytes.hex()} != expected {_land_pixel().hex()}"
    )

    # --- Transparent pixel: tile absent or all-zero bytes at that position ---
    ocean_lon = SRC_WEST + OCEAN_COL + 0.5
    ocean_lat = SRC_NORTH - OCEAN_ROW - 0.5
    ox, oy = _lonlat_to_3857(ocean_lon, ocean_lat)
    otx, oty, opx, opy = _world_to_tile(ox, oy, ZOOM)
    ocean_tile = out_dir / str(ZOOM) / str(otx) / f"{oty}.bin"
    if ocean_tile.is_file():
        with gzip.open(ocean_tile, "rb") as f:
            ocean_raw = f.read()
        ocean_offset = (opy * TILE_SIZE + opx) * BYTES_PER_PIXEL
        ocean_bytes = bytes(ocean_raw[ocean_offset : ocean_offset + BYTES_PER_PIXEL])
        assert ocean_bytes == _transparent_pixel()


def test_overwrites_output_dir(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)
    out_dir.mkdir()
    stale = out_dir / "stale.txt"
    stale.write_text("should be removed")

    rc = main(
        [
            "--input", str(src_tif),
            "--output-dir", str(out_dir),
            "--min-zoom", str(ZOOM),
            "--max-zoom", str(ZOOM),
        ]
    )
    assert rc == 0
    assert not stale.exists()
    assert (out_dir / "tilejson.json").is_file()


PYRAMID_MAX_Z = 10
PYRAMID_MIN_Z = 8


def test_pyramid_produces_tiles_at_each_zoom(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)

    rc = main(
        [
            "--input", str(src_tif),
            "--output-dir", str(out_dir),
            "--min-zoom", str(PYRAMID_MIN_Z),
            "--max-zoom", str(PYRAMID_MAX_Z),
        ]
    )
    assert rc == 0

    tj = json.loads((out_dir / "tilejson.json").read_text())
    assert tj["minzoom"] == PYRAMID_MIN_Z
    assert tj["maxzoom"] == PYRAMID_MAX_Z

    for z in range(PYRAMID_MIN_Z, PYRAMID_MAX_Z + 1):
        zdir = out_dir / str(z)
        assert zdir.is_dir() and any(zdir.iterdir()), f"no tiles at zoom {z}"


def test_pyramid_coarse_pixel_contains_land(tmp_path: Path) -> None:
    """Byte-wise OR downsample: a land pixel at max zoom makes the parent opaque."""
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)

    rc = main(
        [
            "--input", str(src_tif),
            "--output-dir", str(out_dir),
            "--min-zoom", str(PYRAMID_MIN_Z),
            "--max-zoom", str(PYRAMID_MAX_Z),
        ]
    )
    assert rc == 0

    # The parent pixel at PYRAMID_MAX_Z - 1 that contains the known land cell
    # must also be opaque land (flag byte non-zero) after byte-wise OR downsample.
    parent_z = PYRAMID_MAX_Z - 1
    land_lon = SRC_WEST + LAND_COL + 0.5
    land_lat = SRC_NORTH - LAND_ROW - 0.5
    lx, ly = _lonlat_to_3857(land_lon, land_lat)
    ptx, pty, ppx, ppy = _world_to_tile(lx, ly, parent_z)
    tile_path = out_dir / str(parent_z) / str(ptx) / f"{pty}.bin"
    assert tile_path.is_file(), f"expected parent tile at {tile_path}"

    with gzip.open(tile_path, "rb") as f:
        raw = f.read()
    pixel_offset = (ppy * TILE_SIZE + ppx) * BYTES_PER_PIXEL
    pixel_bytes = bytes(raw[pixel_offset : pixel_offset + BYTES_PER_PIXEL])
    # The flag byte must be non-zero (land from the OR of children).
    assert pixel_bytes[_FLAG_BYTE] != 0, (
        f"coarse pixel flag byte is 0 — land child was not ORed in: {pixel_bytes.hex()}"
    )
