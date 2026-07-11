"""CLI entry point for encode_tiles."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Sequence

from encode_tiles.mosaic import (
    TILE_SIZE,
    compute_base_spec,
    iter_base_tile_rows,
    open_mosaic_source,
)
from encode_tiles.tilejson import build_tilejson
from encode_tiles.tiles import build_pyramid


DEFAULT_OUTPUT_DIR = Path("build/tiles")
DEFAULT_MIN_ZOOM = 6
DEFAULT_MAX_ZOOM = 14
DEFAULT_COMPRESS_LEVEL = 6


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="encode_tiles",
        description=(
            "Encode packed bitmask GeoTIFFs into one XYZ .bin tile pyramid per "
            "ADR-0013. Multiple --input boxes are merged into a single tileset "
            "with overlaps resolved by deepest-interior wins (ADR-0015)."
        ),
    )
    parser.add_argument(
        "--input",
        required=True,
        action="append",
        type=Path,
        help=(
            "A packed bitmask GeoTIFF produced by the engine, or a folder of "
            "them (all *.tif inside, sorted by name). Repeatable: all inputs "
            "are merged into one tileset."
        ),
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
    parser.add_argument(
        "--compress-level",
        type=int,
        default=DEFAULT_COMPRESS_LEVEL,
        help=(
            "gzip compression level for .bin tiles, 0-9 (default: "
            f"{DEFAULT_COMPRESS_LEVEL}). 6 compresses ~4x faster than 9 for "
            "~12%% larger tiles; 9 is smallest but slowest."
        ),
    )
    return parser


def _expand_inputs(paths: Sequence[Path]) -> list[Path]:
    """Resolve each --input to concrete GeoTIFF files.

    A file is kept as-is; a directory expands to its `*.tif` contents sorted
    by name (so the merge order — which breaks exact interior-distance ties —
    is deterministic).  Raises ValueError if a path is missing or a folder
    holds no tifs.
    """
    resolved: list[Path] = []
    for path in paths:
        if not path.exists():
            raise ValueError(f"input does not exist: {path}")
        if path.is_dir():
            tifs = sorted(path.glob("*.tif"))
            if not tifs:
                raise ValueError(f"input folder has no .tif files: {path}")
            resolved.extend(tifs)
        elif path.is_file():
            resolved.append(path)
        else:
            raise ValueError(f"input is neither a file nor a folder: {path}")
    return resolved


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        inputs = _expand_inputs(args.input)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
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

    if not 0 <= args.compress_level <= 9:
        print(
            f"error: --compress-level ({args.compress_level}) must be in 0-9",
            file=sys.stderr,
        )
        return 2

    if args.output_dir.exists():
        shutil.rmtree(args.output_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    try:
        with open_mosaic_source(inputs) as (src, contract):
            spec = compute_base_spec(src, contract, zoom=args.max_zoom)
            build_pyramid(
                iter_base_tile_rows(src, spec),
                spec,
                min_zoom=args.min_zoom,
                output_dir=args.output_dir,
                compress_level=args.compress_level,
            )
    except (ValueError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    tilejson = build_tilejson(
        bounds=spec.data_bounds_4326,
        minzoom=args.min_zoom,
        maxzoom=args.max_zoom,
        tile_size=TILE_SIZE,
        format_version=spec.format_version,
        azimuth_min_deg=spec.azimuth_min_deg,
        azimuth_max_deg=spec.azimuth_max_deg,
        azimuth_step_deg=spec.azimuth_step_deg,
        bit_count=spec.bit_count,
        bytes_per_pixel=spec.bytes_per_pixel,
    )
    (args.output_dir / "tilejson.json").write_text(json.dumps(tilejson, indent=2))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
