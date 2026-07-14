"""Build the two US-boundary polygons the map needs.

Usual run (every time the Processing Boxes change — invoked automatically by
the run_boxes driver when the manifest sets "fill_dir"):

    python3 scripts/build_us_polygons.py --boxes boxes.json --out-dir frontend/

This mode is fully self-contained: it reuses the existing
``<out-dir>/us_land.geojson`` as the US outline and extracts ocean water
straight from the engine's own OSM water-polygons shapefile
(``--osm-water-shp``, default ``data/OSM/water-polygons-split-4326/…``) via
``ogr2ogr`` — no downloads, no hand-run extraction.

Rare run (only to rebuild the base US outline itself):

    python3 scripts/build_us_polygons.py \
        --countries <ne_10m_admin_0_countries.geojson> \
        --boxes boxes.json --out-dir frontend/

Produces two GeoJSON files:

* ``us_land.geojson`` — the contiguous US plus Hawaii (Alaska deliberately
  dropped: the map treats it as not part of the USA).  The tile pipeline
  clips box output to it (``encode_tiles --us-boundary``).

* ``us_land_fill.geojson`` — ``us_land`` minus the Processing Boxes minus
  **OSM ocean water** (the same water-polygons dataset the engine's ocean
  mask uses).  The frontend paints this black as the "non-visible" base,
  giving uncovered territory the exact semantics of a tile pixel: everything
  in the US that is not ocean water — including lakes, rivers, and Delta
  channels — is land and therefore non-visible; ocean water is a hole and
  shows the basemap, just like tile-transparent ocean.  The ocean geometry is
  subtracted *after* the boxes are cut out, so the detailed coastline inside
  the boxes never bloats the file.

The fill is a **derived artifact of the box manifest**: it records a
provenance hash of its inputs (the box list + the us_land bytes), and
``--if-stale`` exits immediately when the existing fill already matches — so
callers (the run_boxes driver) can invoke this script unconditionally after
every pass and a stale fill can never silently survive a boxes.json edit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from shapely.geometry import box, mapping, shape
from shapely.ops import unary_union

# The contiguous US reaches ~49.4°N; Hawaii ~21°N. Anything whose southern
# extent is north of this is Alaska (or an Aleutian/Arctic islet) and is dropped.
ALASKA_MIN_LAT = 50.0
# Coastline simplification tolerance in degrees (~400 m) for the US outline.
SIMPLIFY_TOL = 0.004
COORD_DECIMALS = 4
# Stamp format for the fill's provenance foreign member.
PROVENANCE_VERSION = 1
# Degrees of slack around the boxes' bounding rect for the ocean extraction.
OCEAN_EXTRACT_MARGIN = 1.0

DEFAULT_OSM_WATER_SHP = Path("data/OSM/water-polygons-split-4326/water_polygons.shp")


def _round_geojson(geom_mapping: dict) -> dict:
    """Round every coordinate to COORD_DECIMALS in place (shrinks the file)."""

    def r(obj):
        if isinstance(obj, (list, tuple)):
            if obj and isinstance(obj[0], (int, float)):
                return [round(float(obj[0]), COORD_DECIMALS), round(float(obj[1]), COORD_DECIMALS)]
            return [r(x) for x in obj]
        return obj

    return {**geom_mapping, "coordinates": r(geom_mapping["coordinates"])}


def _feature_collection(geom_mapping: dict, name: str) -> dict:
    return {
        "type": "FeatureCollection",
        "features": [
            {
                "type": "Feature",
                "properties": {"name": name, "source": "Natural Earth 10m admin-0 (simplified)"},
                "geometry": geom_mapping,
            }
        ],
    }


def _drop_alaska(us_geom):
    """Return the US land polygon(s) with Alaska removed (see ALASKA_MIN_LAT)."""
    parts = list(us_geom.geoms) if us_geom.geom_type == "MultiPolygon" else [us_geom]
    kept = [p for p in parts if p.bounds[1] < ALASKA_MIN_LAT]  # bounds = (minx, miny, maxx, maxy)
    return unary_union(kept)


def _box_bounds(boxes_doc: dict) -> list[list[float]]:
    """The manifest's boxes as [north, west, south, east] rows, in order."""
    out = []
    for entry in boxes_doc["boxes"]:
        north, west = entry["top_left"]
        south, east = entry["bottom_right"]
        out.append([float(north), float(west), float(south), float(east)])
    return out


def _box_union(boxes_doc: dict):
    """Union of every Processing Box rectangle as one geometry."""
    rects = []
    for north, west, south, east in _box_bounds(boxes_doc):
        rects.append(box(west, south, east, north))
    return unary_union(rects)


def _inputs_hash(boxes_doc: dict, us_land_bytes: bytes) -> str:
    """The fill's freshness key: the box list plus the US outline it was cut from."""
    payload = json.dumps(
        {
            "boxes": _box_bounds(boxes_doc),
            "us_land_sha256": hashlib.sha256(us_land_bytes).hexdigest(),
        },
        sort_keys=True,
    )
    return "sha256:" + hashlib.sha256(payload.encode()).hexdigest()


def _extract_ocean(shp: Path, boxes_doc: dict, dst: Path) -> None:
    """ogr2ogr the OSM water polygons around the boxes' bounding rect."""
    bounds = _box_bounds(boxes_doc)
    west = min(b[1] for b in bounds) - OCEAN_EXTRACT_MARGIN
    east = max(b[3] for b in bounds) + OCEAN_EXTRACT_MARGIN
    south = min(b[2] for b in bounds) - OCEAN_EXTRACT_MARGIN
    north = max(b[0] for b in bounds) + OCEAN_EXTRACT_MARGIN
    subprocess.run(
        [
            "ogr2ogr", "-f", "GeoJSON",
            "-spat", str(west), str(south), str(east), str(north),
            str(dst), str(shp),
        ],
        check=True,
    )


def _ocean_union(paths: list[Path]):
    """Union of the OSM ocean-water polygons in `paths`.

    Lightly pre-simplified (~10 m — under a z14 pixel) so the union stays
    cheap without visibly moving a shoreline.
    """
    geoms = []
    for path in paths:
        doc = json.loads(path.read_text())
        for feat in doc.get("features", []):
            geom = feat.get("geometry")
            if not geom:
                continue
            g = shape(geom)
            if not g.is_valid:
                g = g.buffer(0)
            geoms.append(g.simplify(0.0001, preserve_topology=True))
    return unary_union(geoms) if geoms else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--countries", type=Path, default=None,
                        help="Natural Earth ne_10m_admin_0_countries.geojson — only to "
                             "REBUILD the base US outline; omit to reuse the existing "
                             "<out-dir>/us_land.geojson.")
    parser.add_argument("--boxes", type=Path, required=True, help="boxes.json manifest")
    parser.add_argument("--ocean-water", type=Path, action="append", default=[],
                        help="Pre-extracted GeoJSON of OSM ocean water (repeatable); "
                             "overrides the --osm-water-shp auto-extraction.")
    parser.add_argument("--osm-water-shp", type=Path, default=DEFAULT_OSM_WATER_SHP,
                        help="The engine's OSM water-polygons shapefile to auto-extract "
                             f"ocean water from (default: {DEFAULT_OSM_WATER_SHP}).")
    parser.add_argument("--out-dir", type=Path, required=True,
                        help="where to write the two files")
    parser.add_argument("--if-stale", action="store_true",
                        help="Exit 0 immediately when the existing fill's provenance "
                             "already matches the boxes and US outline.")
    args = parser.parse_args()

    boxes_doc = json.loads(args.boxes.read_text())
    us_land_path = args.out_dir / "us_land.geojson"
    fill_path = args.out_dir / "us_land_fill.geojson"

    if args.countries is None and not us_land_path.is_file():
        print(
            f"error: {us_land_path} not found — run once with --countries to build "
            "the base US outline first",
            file=sys.stderr,
        )
        return 2

    if args.if_stale and args.countries is None and fill_path.is_file():
        try:
            have = json.loads(fill_path.read_text()).get("provenance", {})
        except (OSError, json.JSONDecodeError):
            have = {}
        if have.get("inputs_sha256") == _inputs_hash(boxes_doc, us_land_path.read_bytes()):
            print(f"{fill_path} is fresh — nothing to do")
            return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.countries is not None:
        countries = json.loads(args.countries.read_text())
        us_feat = next(
            f for f in countries["features"]
            if f["properties"].get("ADMIN") == "United States of America"
            and f["properties"].get("TYPE") == "Country"
        )
        us = _drop_alaska(shape(us_feat["geometry"])).simplify(SIMPLIFY_TOL)
        us_land_path.write_text(
            json.dumps(
                _feature_collection(_round_geojson(mapping(us)), "United States (excl. Alaska)"),
                separators=(",", ":"),
            )
        )
        # Reload what was written so the geometry and the provenance hash both
        # describe the rounded, serialized outline — not the in-memory one.

    us = shape(json.loads(us_land_path.read_text())["features"][0]["geometry"])

    boxes_union = _box_union(boxes_doc)
    fill = us.difference(boxes_union)

    if args.ocean_water:
        ocean = _ocean_union(args.ocean_water)
    else:
        if not args.osm_water_shp.is_file():
            print(
                f"error: OSM water shapefile not found: {args.osm_water_shp} "
                "(pass --osm-water-shp or --ocean-water)",
                file=sys.stderr,
            )
            return 2
        if shutil.which("ogr2ogr") is None:
            print(
                "error: ogr2ogr not on PATH (install GDAL, or pass a pre-extracted "
                "--ocean-water GeoJSON)",
                file=sys.stderr,
            )
            return 2
        with tempfile.TemporaryDirectory(prefix="us_polygons_") as tmp:
            extract = Path(tmp) / "ocean.geojson"
            _extract_ocean(args.osm_water_shp, boxes_doc, extract)
            ocean = _ocean_union([extract])

    if ocean is not None:
        # Cut the boxes out of the ocean first: the fill already excludes the
        # boxes, so only the ocean OUTSIDE them can matter — this drops the
        # detailed in-box coastline (most of it) before the expensive subtract.
        fill = fill.difference(ocean.difference(boxes_union))

    fill_fc = _feature_collection(
        _round_geojson(mapping(fill)), "US land outside Processing Boxes"
    )
    fill_fc["provenance"] = {
        "version": PROVENANCE_VERSION,
        "inputs_sha256": _inputs_hash(boxes_doc, us_land_path.read_bytes()),
    }
    fill_path.write_text(json.dumps(fill_fc, separators=(",", ":")))

    print(f"{us_land_path.name}: {us.geom_type}, bounds={tuple(round(b, 2) for b in us.bounds)}")
    print(f"{fill_path.name}: {fill.geom_type}, bounds={tuple(round(b, 2) for b in fill.bounds)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
