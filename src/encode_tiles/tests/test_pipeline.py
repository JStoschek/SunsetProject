"""End-to-end test for the single-zoom encoder pipeline."""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest
import rasterio
from PIL import Image
from rasterio.transform import from_bounds

from encode_tiles.cli import main
from encode_tiles.mosaic import TILE_SIZE, WEB_MERCATOR_EXTENT


SRC_HEIGHT = 4
SRC_WEST = 0.0
SRC_EAST = 4.0
SRC_SOUTH = 0.0
SRC_NORTH = 4.0
# Row 0 is the northern row. Layout:
#   col:    0      1      2      3
#  row 0: (210,230) (233,250) (309,320) (200,220)
#  row 1: (220,240)  NaN      (250,270) (310,340)
#  row 2: (230,250) (240,260) (260,280) (320,350)
#  row 3: (240,260) (250,270) (270,290) (330,360)
LAND_ROW, LAND_COL = 0, 0
LAND_MIN_AZ, LAND_MAX_AZ = 210.0, 230.0
OCEAN_ROW, OCEAN_COL = 1, 1
ZOOM = 10


def _fixture_arrays() -> tuple[np.ndarray, np.ndarray]:
    min_az = np.array(
        [
            [210.0, 233.0, 309.0, 200.0],
            [220.0, np.nan, 250.0, 310.0],
            [230.0, 240.0, 260.0, 320.0],
            [240.0, 250.0, 270.0, 330.0],
        ],
        dtype=np.float32,
    )
    max_az = np.array(
        [
            [230.0, 250.0, 320.0, 220.0],
            [240.0, np.nan, 270.0, 340.0],
            [250.0, 260.0, 280.0, 350.0],
            [260.0, 270.0, 290.0, 360.0],
        ],
        dtype=np.float32,
    )
    return min_az, max_az


def _make_fixture(path: Path) -> None:
    min_az, max_az = _fixture_arrays()
    transform = from_bounds(SRC_WEST, SRC_SOUTH, SRC_EAST, SRC_NORTH, 4, 4)
    with rasterio.open(
        path,
        "w",
        driver="GTiff",
        width=4,
        height=4,
        count=2,
        dtype="float32",
        crs="EPSG:4326",
        transform=transform,
        nodata=float("nan"),
    ) as dst:
        dst.write(min_az, 1)
        dst.write(max_az, 2)


def _lonlat_to_3857(lon: float, lat: float) -> tuple[float, float]:
    x = math.radians(lon) * 6378137.0
    y = math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)) * 6378137.0
    return x, y


def _world_to_tile(x: float, y: float, zoom: int) -> tuple[int, int, int, int]:
    """3857 coords -> (tile_x, tile_y, pixel_x_in_tile, pixel_y_in_tile)."""
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
            "--input",
            str(src_tif),
            "--output-dir",
            str(out_dir),
            "--min-zoom",
            str(ZOOM),
            "--max-zoom",
            str(ZOOM),
        ]
    )
    assert rc == 0

    # --- TileJSON ---
    tj_path = out_dir / "tilejson.json"
    assert tj_path.is_file()
    tj = json.loads(tj_path.read_text())
    assert tj["minzoom"] == ZOOM
    assert tj["maxzoom"] == ZOOM
    assert tj["tile_size"] == TILE_SIZE
    west, south, east, north = tj["bounds"]
    assert west == pytest.approx(SRC_WEST, abs=1e-6)
    assert south == pytest.approx(SRC_SOUTH, abs=1e-6)
    assert east == pytest.approx(SRC_EAST, abs=1e-6)
    assert north == pytest.approx(SRC_NORTH, abs=1e-6)

    # --- Known land pixel round-trip ---
    # Source row r corresponds to lat band [SRC_NORTH - (r+1), SRC_NORTH - r].
    land_lon = SRC_WEST + LAND_COL + 0.5
    land_lat = SRC_NORTH - LAND_ROW - 0.5
    lx, ly = _lonlat_to_3857(land_lon, land_lat)
    tx, ty, px, py = _world_to_tile(lx, ly, ZOOM)
    tile_path = out_dir / str(ZOOM) / str(tx) / f"{ty}.png"
    assert tile_path.is_file(), f"expected land tile at {tile_path}"

    arr = np.asarray(Image.open(tile_path).convert("RGBA"))
    r, g, _b, a = arr[py, px]
    assert a == 255
    decoded_min = r / 255.0 * 160.0 + 200.0
    decoded_max = g / 255.0 * 160.0 + 200.0
    assert abs(decoded_min - LAND_MIN_AZ) <= 0.6
    assert abs(decoded_max - LAND_MAX_AZ) <= 0.6

    # --- Ocean pixel has A = 0 ---
    ocean_lon = SRC_WEST + OCEAN_COL + 0.5
    ocean_lat = SRC_NORTH - OCEAN_ROW - 0.5
    ox, oy = _lonlat_to_3857(ocean_lon, ocean_lat)
    otx, oty, opx, opy = _world_to_tile(ox, oy, ZOOM)
    ocean_tile = out_dir / str(ZOOM) / str(otx) / f"{oty}.png"
    if ocean_tile.is_file():
        arr_o = np.asarray(Image.open(ocean_tile).convert("RGBA"))
        assert arr_o[opy, opx, 3] == 0
    # else: tile fully transparent and was omitted — also acceptable.


def test_overwrites_output_dir(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)
    out_dir.mkdir()
    stale = out_dir / "stale.txt"
    stale.write_text("should be removed")

    rc = main(
        [
            "--input",
            str(src_tif),
            "--output-dir",
            str(out_dir),
            "--min-zoom",
            str(ZOOM),
            "--max-zoom",
            str(ZOOM),
        ]
    )
    assert rc == 0
    assert not stale.exists()
    assert (out_dir / "tilejson.json").is_file()


def test_invalid_source_pixel_fails(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    # Out-of-range min_az (190 < 200) at (col=1, row=2).
    min_az = np.full((4, 4), 210.0, dtype=np.float32)
    max_az = np.full((4, 4), 250.0, dtype=np.float32)
    min_az[2, 1] = 190.0
    transform = from_bounds(SRC_WEST, SRC_SOUTH, SRC_EAST, SRC_NORTH, 4, 4)
    with rasterio.open(
        src_tif,
        "w",
        driver="GTiff",
        width=4,
        height=4,
        count=2,
        dtype="float32",
        crs="EPSG:4326",
        transform=transform,
        nodata=float("nan"),
    ) as dst:
        dst.write(min_az, 1)
        dst.write(max_az, 2)

    rc = main(
        [
            "--input",
            str(src_tif),
            "--output-dir",
            str(out_dir),
            "--min-zoom",
            str(ZOOM),
            "--max-zoom",
            str(ZOOM),
        ]
    )
    assert rc != 0
    err = capsys.readouterr().err
    assert "x=1" in err and "y=2" in err
    assert "190" in err


PYRAMID_MAX_Z = 10
PYRAMID_MIN_Z = 8


def _pixel_center_lonlat(
    z: int, tx: int, ty: int, px: int, py: int
) -> tuple[float, float]:
    te = 2.0 * WEB_MERCATOR_EXTENT / (2 ** z)
    pix = te / TILE_SIZE
    x = -WEB_MERCATOR_EXTENT + (tx * TILE_SIZE + px + 0.5) * pix
    y = WEB_MERCATOR_EXTENT - (ty * TILE_SIZE + py + 0.5) * pix
    lon = math.degrees(x / 6378137.0)
    lat = math.degrees(2.0 * math.atan(math.exp(y / 6378137.0)) - math.pi / 2.0)
    return lon, lat


def _source_rgba_at_lonlat(
    lon: float, lat: float, min_az: np.ndarray, max_az: np.ndarray
) -> tuple[int, int, int, int]:
    if not (SRC_WEST <= lon < SRC_EAST and SRC_SOUTH <= lat < SRC_NORTH):
        return (0, 0, 0, 0)
    col = int(lon - SRC_WEST)
    row = int(SRC_NORTH - lat)
    mn = float(min_az[row, col])
    mx = float(max_az[row, col])
    if math.isnan(mn) or math.isnan(mx):
        return (0, 0, 0, 0)
    r = round((mn - 200.0) / 160.0 * 255)
    g = round((mx - 200.0) / 160.0 * 255)
    return (r, g, 0, 255)


def test_pyramid_pipeline(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)
    fixture_min, fixture_max = _fixture_arrays()

    rc = main(
        [
            "--input",
            str(src_tif),
            "--output-dir",
            str(out_dir),
            "--min-zoom",
            str(PYRAMID_MIN_Z),
            "--max-zoom",
            str(PYRAMID_MAX_Z),
        ]
    )
    assert rc == 0

    tj = json.loads((out_dir / "tilejson.json").read_text())
    assert tj["minzoom"] == PYRAMID_MIN_Z
    assert tj["maxzoom"] == PYRAMID_MAX_Z

    for z in range(PYRAMID_MIN_Z, PYRAMID_MAX_Z + 1):
        zdir = out_dir / str(z)
        assert zdir.is_dir() and any(zdir.iterdir()), f"no tiles at zoom {z}"

    # --- Parent pixel at MAX_Z - 1 over deep land cell (row=0, col=0). ---
    parent_z = PYRAMID_MAX_Z - 1
    land_lon = SRC_WEST + LAND_COL + 0.5
    land_lat = SRC_NORTH - LAND_ROW - 0.5
    lx, ly = _lonlat_to_3857(land_lon, land_lat)
    ptx, pty, ppx, ppy = _world_to_tile(lx, ly, parent_z)
    tile_path = out_dir / str(parent_z) / str(ptx) / f"{pty}.png"
    assert tile_path.is_file(), f"expected parent tile at {tile_path}"
    arr = np.asarray(Image.open(tile_path).convert("RGBA"))
    pr, pg, _pb, pa = arr[ppy, ppx]
    assert pa == 255

    # Independently compute the alpha-weighted mean of the four MAX_Z children
    # that cover this parent pixel, by sampling the source fixture.
    gx = ptx * TILE_SIZE + ppx
    gy = pty * TILE_SIZE + ppy
    children = []
    for dy in (0, 1):
        for dx in (0, 1):
            cgx, cgy = 2 * gx + dx, 2 * gy + dy
            ctx, cpx = divmod(cgx, TILE_SIZE)
            cty, cpy = divmod(cgy, TILE_SIZE)
            lon, lat = _pixel_center_lonlat(PYRAMID_MAX_Z, ctx, cty, cpx, cpy)
            children.append(
                _source_rgba_at_lonlat(lon, lat, fixture_min, fixture_max)
            )
    total_a = sum(a for *_, a in children)
    assert total_a > 0
    exp_r = sum(r * a for r, _g, _b, a in children) / total_a
    exp_g = sum(g * a for _r, g, _b, a in children) / total_a
    assert abs(pr - exp_r) / 255.0 * 160.0 <= 0.6
    assert abs(pg - exp_g) / 255.0 * 160.0 <= 0.6

    # --- All-transparent parent pixel: deep inside the NaN ocean cell (1,1). ---
    ocean_lon = SRC_WEST + OCEAN_COL + 0.5
    ocean_lat = SRC_NORTH - OCEAN_ROW - 0.5
    olx, oly = _lonlat_to_3857(ocean_lon, ocean_lat)
    otx, oty, opx, opy = _world_to_tile(olx, oly, parent_z)
    ocean_tile = out_dir / str(parent_z) / str(otx) / f"{oty}.png"
    if ocean_tile.is_file():
        arr_o = np.asarray(Image.open(ocean_tile).convert("RGBA"))
        assert arr_o[opy, opx, 3] == 0
