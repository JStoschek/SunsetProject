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

The frontend's `us_land_fill.geojson` is a **derived artifact of this
manifest** (it has the boxes cut out of it), so a boxes edit silently breaks
the map unless the fill is rebuilt.  When the manifest sets `fill_dir` (or
`--fill-dir` is passed), the driver re-runs
`scripts/build_us_polygons.py --if-stale` after every successful pass; the
script's own provenance stamp makes that a no-op when nothing changed, and a
regeneration failure fails the run loudly instead of leaving a stale fill.

Runs are **incremental** (ADR-0017): after each engine success the driver
writes a provenance sidecar `box_NNN.tif.json` recording the box's bounds and
a hash of `pipeline.conf`.  On the next run a box whose tif and matching
sidecar both exist is skipped — nothing the driver controls changed.  The DEM
rasters and the engine binary are deliberately *outside* this freshness key;
after re-acquiring DEM tiles or rebuilding the engine, pass `--force` to
reprocess every box.  When the manifest shrinks, trailing `box_*` outputs
beyond the new length are pruned so the merge folder stays honest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

# The provenance sidecar schema version (ADR-0017). Bump when its shape
# changes; an unrecognised version is treated as a miss (reprocess), never an
# error.
SIDECAR_VERSION = 1

# box_000.tif / box_012.tif.json → capture the positional index for pruning,
# tolerant of a change in zero-pad width.
_BOX_INDEX_RE = re.compile(r"^box_(\d+)\.tif(?:\.json)?$")

# The fill-polygon builder, resolved relative to the repo root so the driver
# works from any cwd. Overridable via --fill-script (mainly for tests).
_DEFAULT_FILL_SCRIPT = (
    Path(__file__).resolve().parents[2] / "scripts" / "build_us_polygons.py"
)


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
    fill_dir: Path | None
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
        fill_dir=_opt_path("fill_dir"),
        boxes=boxes,
    )


def _config_hash(config: Path) -> str:
    """SHA-256 of the resolved pipeline.conf bytes, the config half of the key."""
    return "sha256:" + hashlib.sha256(config.read_bytes()).hexdigest()


def _sidecar_path(out_path: Path) -> Path:
    """The provenance record next to a box tif: box_NNN.tif.json (ADR-0017)."""
    return out_path.with_name(out_path.name + ".json")


def _read_sidecar(path: Path) -> dict | None:
    """Load a provenance sidecar; any unreadable/malformed file is a miss."""
    try:
        doc = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    return doc if isinstance(doc, dict) else None


def _is_fresh(box: Box, out_path: Path, config_hash: str) -> bool:
    """True iff the tif and a matching sidecar both exist for this box.

    Freshness compares *inputs* — the box's four bounds and the config hash —
    so a box is skipped only when nothing the driver controls has changed.
    """
    if not out_path.is_file():
        return False
    doc = _read_sidecar(_sidecar_path(out_path))
    if doc is None or doc.get("version") != SIDECAR_VERSION:
        return False
    return (
        doc.get("north") == box.north
        and doc.get("west") == box.west
        and doc.get("south") == box.south
        and doc.get("east") == box.east
        and doc.get("config_hash") == config_hash
    )


def _write_sidecar(box: Box, out_path: Path, config_hash: str) -> None:
    """Record what produced this tif, written only after an engine success."""
    _sidecar_path(out_path).write_text(
        json.dumps(
            {
                "version": SIDECAR_VERSION,
                "north": box.north,
                "west": box.west,
                "south": box.south,
                "east": box.east,
                "config_hash": config_hash,
            }
        )
    )


def _prune_orphans(output_dir: Path, box_count: int) -> list[str]:
    """Delete box tif/sidecar files whose index is beyond the manifest.

    Called only after a pass that finished with no engine failure. A shorter
    manifest renumbers the tail (ADR-0015 names by position), so any
    `box_*.tif`/`.tif.json` with index >= box_count is a leftover from a longer
    previous run and would otherwise be merged by encode_tiles. Returns the
    names removed, for the caller to report.
    """
    removed: list[str] = []
    for path in output_dir.iterdir():
        match = _BOX_INDEX_RE.match(path.name)
        if match is not None and int(match.group(1)) >= box_count:
            path.unlink()
            removed.append(path.name)
    return sorted(removed)


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
    parser.add_argument(
        "--fill-dir",
        type=Path,
        default=None,
        help=(
            "Folder holding us_land_fill.geojson to regenerate after a "
            "successful pass (overrides the manifest's 'fill_dir'). The fill "
            "has the boxes cut out of it, so it goes stale whenever the "
            "manifest changes."
        ),
    )
    parser.add_argument(
        "--fill-script",
        type=Path,
        default=None,
        help=(
            "Path to the fill-polygon builder "
            f"(default: {_DEFAULT_FILL_SCRIPT})."
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help=(
            "Reprocess every box even if its bounds and config are unchanged. "
            "Use after a DEM re-acquisition or an engine rebuild, which are "
            "outside the freshness key (ADR-0017)."
        ),
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

    config_hash = _config_hash(Path(config))

    total = len(manifest.boxes)
    for i, box in enumerate(manifest.boxes, start=1):
        out_path = output_dir / f"{box.name}.tif"

        if not args.force and _is_fresh(box, out_path, config_hash):
            print(
                f"[{i}/{total}] {box.name}: "
                f"N{box.north} W{box.west} S{box.south} E{box.east} "
                "-- unchanged, skipping",
                flush=True,
            )
            continue

        cmd = [
            str(engine),
            "--config", str(config),
            *box.bbox_args(),
            "--output", str(out_path),
        ]
        print(
            f"[{i}/{total}] {box.name}: "
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

        # Record what produced this tif only now the engine has succeeded, so a
        # crash never leaves a false "done" marker (ADR-0017).
        _write_sidecar(box, out_path, config_hash)

    # The pass finished with no engine failure: outputs from a longer previous
    # run are now orphans encode_tiles would still merge. Prune them.
    pruned = _prune_orphans(output_dir, total)
    if pruned:
        print(
            f"Pruned {len(pruned)} orphaned file(s) beyond the manifest: "
            + ", ".join(pruned),
            flush=True,
        )

    # The frontend fill polygon has these boxes cut out of it, so it is stale
    # the moment the manifest changes — rebuild it now. The script's own
    # --if-stale provenance check makes this a fast no-op when nothing moved.
    fill_dir = args.fill_dir or manifest.fill_dir
    if fill_dir is not None:
        fill_script = args.fill_script or _DEFAULT_FILL_SCRIPT
        fill_cmd = [
            sys.executable, str(fill_script),
            "--boxes", str(args.manifest),
            "--out-dir", str(fill_dir),
            "--if-stale",
        ]
        if subprocess.run(fill_cmd).returncode != 0:
            print(
                "error: us_land_fill.geojson regeneration failed — the frontend "
                "fill no longer matches these boxes and the map will render "
                "stale coverage. Fix and re-run: " + " ".join(fill_cmd),
                file=sys.stderr,
            )
            return 1

    print(
        f"Wrote {len(manifest.boxes)} box tif(s) to {output_dir}. "
        f"Encode with: python3 -m encode_tiles --input {output_dir} "
        "--output-dir frontend/tiles/ --us-boundary frontend/us_land.geojson",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
