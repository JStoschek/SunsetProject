"""CLI entry point for encode_tiles."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Sequence

from encode_tiles.mosaic import TILE_SIZE, encode_source_to_base_raster
from encode_tiles.tilejson import build_tilejson
from encode_tiles.tiles import build_pyramid


DEFAULT_OUTPUT_DIR = Path("build/tiles")
DEFAULT_MIN_ZOOM = 6
DEFAULT_MAX_ZOOM = 14


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="encode_tiles",
        description=(
            "Encode a two-band float32 GeoTIFF (min_az, max_az) into an XYZ "
            "RGBA PNG tile pyramid per ADR-0003."
        ),
    )
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        help="Path to the two-band float32 GeoTIFF produced by compute_azimuth_range.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help=f"Directory to write the tile pyramid into (default: {DEFAULT_OUTPUT_DIR}).",
    )
    parser.add_argument(
        "--min-zoom",
        type=int,
        default=DEFAULT_MIN_ZOOM,
        help=f"Minimum (coarsest) zoom level to emit (default: {DEFAULT_MIN_ZOOM}).",
    )
    parser.add_argument(
        "--max-zoom",
        type=int,
        default=DEFAULT_MAX_ZOOM,
        help=f"Maximum (finest) zoom level to emit (default: {DEFAULT_MAX_ZOOM}).",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if not args.input.exists():
        print(f"error: input file does not exist: {args.input}", file=sys.stderr)
        return 2

    if not args.input.is_file():
        print(f"error: input path is not a file: {args.input}", file=sys.stderr)
        return 2

    if args.min_zoom < 0 or args.max_zoom < 0:
        print(
            f"error: zoom levels must be non-negative (got min={args.min_zoom}, max={args.max_zoom})",
            file=sys.stderr,
        )
        return 2

    if args.min_zoom > args.max_zoom:
        print(
            f"error: --min-zoom ({args.min_zoom}) must be <= --max-zoom ({args.max_zoom})",
            file=sys.stderr,
        )
        return 2

    if args.output_dir.exists():
        shutil.rmtree(args.output_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    try:
        mosaic = encode_source_to_base_raster(args.input, zoom=args.max_zoom)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    build_pyramid(mosaic, min_zoom=args.min_zoom, output_dir=args.output_dir)

    tilejson = build_tilejson(
        bounds=mosaic.data_bounds_4326,
        minzoom=args.min_zoom,
        maxzoom=args.max_zoom,
        tile_size=TILE_SIZE,
    )
    (args.output_dir / "tilejson.json").write_text(json.dumps(tilejson, indent=2))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
