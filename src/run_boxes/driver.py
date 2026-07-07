"""Read a boxes manifest and run the engine once per box into a folder.

The manifest (`boxes.json`) is self-contained so that one command runs the
whole set:

    {
      "engine": "build-release/src/compute_azimuth_range/compute_azimuth_range",
      "config": "config/pipeline.conf",
      "output_dir": "build/boxes",
      "boxes": [
        {"top_left": [38.60, -123.60], "bottom_right": [37.90, -122.30]},
        {"top_left": [38.08, -123.20], "bottom_right": [37.30, -121.79]}
      ]
    }

Each box is two corners, `[lat, lon]` each — the north-west `top_left` and
south-east `bottom_right`, the same two points that define the engine's
`--bbox <top_lat> <top_lon> <bot_lat> <bot_lon>`, so a box reads the way it is
drawn.  Boxes are unnamed (there can be many); the driver names them by
position — `box_000.tif`, `box_001.tif`, … — zero-padded so the sorted order
`encode_tiles` merges them in matches manifest order (which breaks
interior-distance ties, ADR-0015).

`engine`, `config`, and `output_dir` may be given in the manifest and/or
overridden on the CLI (CLI wins).  A box whose western edge is not offshore
makes the engine hard-error (ADR-0015); the driver stops on the first such
failure and names the box, since a mis-anchored box must not silently drop
out of the coverage.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


@dataclass(frozen=True)
class Box:
    name: str
    north: float
    west: float
    south: float
    east: float

    def bbox_args(self) -> list[str]:
        """The engine's --bbox operands: top_lat top_lon bot_lat bot_lon."""
        return [
            "--bbox",
            repr(self.north),
            repr(self.west),
            repr(self.south),
            repr(self.east),
        ]


@dataclass(frozen=True)
class Manifest:
    engine: Path | None
    config: Path | None
    output_dir: Path | None
    boxes: list[Box]


def _corner(entry: dict, key: str) -> tuple[float, float]:
    """Read a `[lat, lon]` corner; raise ValueError if it is malformed."""
    val = entry[key]  # KeyError handled by the caller
    if not isinstance(val, (list, tuple)) or len(val) != 2:
        raise ValueError(f"{key} must be a [lat, lon] pair")
    return float(val[0]), float(val[1])


def load_manifest(path: Path) -> Manifest:
    """Parse a boxes manifest; raise ValueError on a malformed document."""
    try:
        doc = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(doc, dict):
        raise ValueError(f"{path}: manifest must be a JSON object")

    raw_boxes = doc.get("boxes")
    if not isinstance(raw_boxes, list) or not raw_boxes:
        raise ValueError(f"{path}: manifest must list at least one box in 'boxes'")

    # Zero-pad the auto-name to a fixed width so the sorted order encode_tiles
    # merges the folder in matches manifest order (the tie-break, ADR-0015).
    width = max(3, len(str(len(raw_boxes) - 1)))

    boxes: list[Box] = []
    for i, entry in enumerate(raw_boxes):
        if not isinstance(entry, dict):
            raise ValueError(f"{path}: box {i} must be a JSON object")
        try:
            top_lat, top_lon = _corner(entry, "top_left")
            bot_lat, bot_lon = _corner(entry, "bottom_right")
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(
                f"{path}: box {i} needs top_left and bottom_right "
                f"[lat, lon] corners ({exc})"
            ) from exc
        boxes.append(
            Box(
                name=f"box_{i:0{width}d}",
                north=top_lat,
                west=top_lon,
                south=bot_lat,
                east=bot_lon,
            )
        )

    def _opt_path(key: str) -> Path | None:
        val = doc.get(key)
        return Path(val) if val is not None else None

    return Manifest(
        engine=_opt_path("engine"),
        config=_opt_path("config"),
        output_dir=_opt_path("output_dir"),
        boxes=boxes,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="run_boxes",
        description=(
            "Run compute_azimuth_range once per box from a boxes.json manifest, "
            "writing one <name>.tif per box into an output folder for "
            "encode_tiles to merge (ADR-0015)."
        ),
    )
    parser.add_argument(
        "manifest",
        type=Path,
        help="Path to the boxes.json manifest.",
    )
    parser.add_argument(
        "--engine",
        type=Path,
        default=None,
        help="Path to the compute_azimuth_range binary (overrides the manifest).",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Path to pipeline.conf (overrides the manifest).",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Folder to write per-box tifs into (overrides the manifest).",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if not args.manifest.is_file():
        print(f"error: manifest not found: {args.manifest}", file=sys.stderr)
        return 2

    try:
        manifest = load_manifest(args.manifest)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    engine = args.engine or manifest.engine
    config = args.config or manifest.config
    output_dir = args.output_dir or manifest.output_dir

    missing = [
        name
        for name, val in (
            ("engine", engine),
            ("config", config),
            ("output_dir", output_dir),
        )
        if val is None
    ]
    if missing:
        print(
            "error: "
            + ", ".join(missing)
            + " must be set in the manifest or on the command line",
            file=sys.stderr,
        )
        return 2

    if not Path(engine).exists():
        print(f"error: engine binary not found: {engine}", file=sys.stderr)
        return 2
    if not Path(config).is_file():
        print(f"error: config not found: {config}", file=sys.stderr)
        return 2

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for i, box in enumerate(manifest.boxes, start=1):
        out_path = output_dir / f"{box.name}.tif"
        cmd = [
            str(engine),
            "--config", str(config),
            *box.bbox_args(),
            "--output", str(out_path),
        ]
        print(
            f"[{i}/{len(manifest.boxes)}] {box.name}: "
            f"N{box.north} W{box.west} S{box.south} E{box.east} -> {out_path}",
            flush=True,
        )
        result = subprocess.run(cmd)
        if result.returncode != 0:
            print(
                f"error: box {box.name!r} failed (engine exit {result.returncode}); "
                "stopping. Fix or remove this box before re-running.",
                file=sys.stderr,
            )
            return 1

    print(
        f"Wrote {len(manifest.boxes)} box tif(s) to {output_dir}. "
        f"Encode with: python3 -m encode_tiles --input {output_dir} "
        "--output-dir frontend/tiles/",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
