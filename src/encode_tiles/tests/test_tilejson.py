"""Tests for build_tilejson pure function."""

from __future__ import annotations

import json

from encode_tiles.tilejson import build_tilejson

BOUNDS = (-120.0, 35.0, -114.0, 42.0)


def test_required_keys_present() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    for key in ("tilejson", "tiles", "scheme", "bounds", "minzoom", "maxzoom"):
        assert key in doc, f"missing required key: {key}"


def test_tilejson_version() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    assert doc["tilejson"] == "3.0.0"


def test_scheme_xyz() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    assert doc["scheme"] == "xyz"


def test_tiles_relative_template() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    assert doc["tiles"] == ["{z}/{x}/{y}.png"]


def test_bounds_west_south_east_north_order() -> None:
    west, south, east, north = -120.0, 35.0, -114.0, 42.0
    doc = build_tilejson((west, south, east, north), minzoom=6, maxzoom=14)
    assert doc["bounds"] == [west, south, east, north]
    assert len(doc["bounds"]) == 4


def test_minzoom_maxzoom_from_args() -> None:
    doc = build_tilejson(BOUNDS, minzoom=3, maxzoom=10)
    assert doc["minzoom"] == 3
    assert doc["maxzoom"] == 10


def test_json_round_trip() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    assert json.loads(json.dumps(doc)) == doc


def test_no_encoding_window_constants() -> None:
    doc = build_tilejson(BOUNDS, minzoom=6, maxzoom=14)
    doc_str = json.dumps(doc)
    assert "200" not in doc_str
    assert "160" not in doc_str
