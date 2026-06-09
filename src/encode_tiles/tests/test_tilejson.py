"""Tests for build_tilejson pure function (sunset-bitmask format, ADR-0013)."""

from __future__ import annotations

import json

from encode_tiles.tilejson import build_tilejson

BOUNDS = (-120.0, 35.0, -114.0, 42.0)

AZ_PARAMS = dict(
    format_version=1,
    azimuth_min_deg=233.0,
    azimuth_max_deg=309.0,
    azimuth_step_deg=1.0,
    bit_count=77,
    bytes_per_pixel=10,
)


def test_required_keys_present() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    for key in ("tilejson", "tiles", "scheme", "bounds", "minzoom", "maxzoom"):
        assert key in doc, f"missing required key: {key}"


def test_tilejson_version() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["tilejson"] == "3.0.0"


def test_scheme_xyz() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["scheme"] == "xyz"


def test_tiles_bin_template() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["tiles"] == ["{z}/{x}/{y}.bin"]


def test_format_sunset_bitmask() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["format"] == "sunset-bitmask"


def test_format_version_passed_through() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["format_version"] == 1


def test_azimuth_params_passed_through() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["azimuth_min_deg"] == 233.0
    assert doc["azimuth_max_deg"] == 309.0
    assert doc["azimuth_step_deg"] == 1.0


def test_bit_count_and_bytes_per_pixel_passed_through() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["bit_count"] == 77
    assert doc["bytes_per_pixel"] == 10


def test_bounds_west_south_east_north_order() -> None:
    west, south, east, north = -120.0, 35.0, -114.0, 42.0
    doc = build_tilejson((west, south, east, north), minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert doc["bounds"] == [west, south, east, north]
    assert len(doc["bounds"]) == 4


def test_minzoom_maxzoom_from_args() -> None:
    doc = build_tilejson(BOUNDS, minzoom=3, maxzoom=10, **AZ_PARAMS)
    assert doc["minzoom"] == 3
    assert doc["maxzoom"] == 10


def test_json_round_trip() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    assert json.loads(json.dumps(doc)) == doc


def test_no_encoding_window_constants() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14, **AZ_PARAMS)
    doc_str = json.dumps(doc)
    assert "200" not in doc_str
    assert "160" not in doc_str
