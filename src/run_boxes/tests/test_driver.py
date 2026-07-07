"""Tests for the run_boxes driver: manifest → one engine run per box.

The engine is faked with a tiny script that records the argv it was called
with and touches the requested output, so the driver's orchestration (which
boxes run, with which bbox, into which folder, and its stop-on-failure
behaviour) is asserted without the real compute or any DEM data.
"""

from __future__ import annotations

import json
import stat
from pathlib import Path

import pytest

from run_boxes.driver import Box, load_manifest, main


def _fake_engine(path: Path, exit_code: int = 0) -> Path:
    """Write an executable that logs its argv and touches --output."""
    log = path.parent / "engine_calls.log"
    script = f"""#!/usr/bin/env python3
import sys
from pathlib import Path
argv = sys.argv[1:]
with open({str(log)!r}, "a") as f:
    f.write(" ".join(argv) + "\\n")
if "--output" in argv:
    out = argv[argv.index("--output") + 1]
    Path(out).write_bytes(b"stub")
sys.exit({exit_code})
"""
    path.write_text(script)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IRUSR)
    return log


def _manifest(path: Path, engine: Path, config: Path, out_dir: Path) -> Path:
    doc = {
        "engine": str(engine),
        "config": str(config),
        "output_dir": str(out_dir),
        "boxes": [
            {"top_left": [38.60, -123.60], "bottom_right": [37.90, -122.30]},
            {"top_left": [38.08, -123.20], "bottom_right": [37.30, -121.79]},
        ],
    }
    path.write_text(json.dumps(doc))
    return path


def test_runs_engine_once_per_box_into_folder(tmp_path: Path) -> None:
    engine = tmp_path / "engine.py"
    log = _fake_engine(engine)
    config = tmp_path / "pipeline.conf"
    config.write_text("stub config")
    out_dir = tmp_path / "boxes"
    manifest = _manifest(tmp_path / "boxes.json", engine, config, out_dir)

    rc = main([str(manifest)])
    assert rc == 0

    # One tif per box, auto-named by position (zero-padded → sorted == order).
    assert (out_dir / "box_000.tif").is_file()
    assert (out_dir / "box_001.tif").is_file()

    # Each invocation carried the right bbox in engine order (top_lat top_lon
    # bot_lat bot_lon = top_left then bottom_right) and its own output path.
    calls = log.read_text().splitlines()
    assert len(calls) == 2
    assert "--bbox 38.6 -123.6 37.9 -122.3" in calls[0]
    assert str(out_dir / "box_000.tif") in calls[0]
    assert "--bbox 38.08 -123.2 37.3 -121.79" in calls[1]
    assert str(out_dir / "box_001.tif") in calls[1]


def test_stops_on_first_failing_box(tmp_path: Path) -> None:
    """A mis-anchored box (engine nonzero) stops the run and reports nonzero."""
    engine = tmp_path / "engine.py"
    log = _fake_engine(engine, exit_code=1)
    config = tmp_path / "pipeline.conf"
    config.write_text("stub")
    out_dir = tmp_path / "boxes"
    manifest = _manifest(tmp_path / "boxes.json", engine, config, out_dir)

    rc = main([str(manifest)])
    assert rc != 0

    # It stopped after the FIRST box — the second never ran.
    calls = log.read_text().splitlines()
    assert len(calls) == 1


def test_cli_overrides_manifest_paths(tmp_path: Path) -> None:
    engine = tmp_path / "engine.py"
    log = _fake_engine(engine)
    config = tmp_path / "pipeline.conf"
    config.write_text("stub")
    override_out = tmp_path / "override_boxes"
    # Manifest points output_dir elsewhere; CLI --output-dir must win.
    manifest = _manifest(
        tmp_path / "boxes.json", engine, config, tmp_path / "manifest_out"
    )

    rc = main([str(manifest), "--output-dir", str(override_out)])
    assert rc == 0
    assert (override_out / "box_000.tif").is_file()
    assert not (tmp_path / "manifest_out").exists()


def test_missing_manifest_exits_nonzero(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    rc = main([str(tmp_path / "nope.json")])
    assert rc == 2
    assert "manifest not found" in capsys.readouterr().err


def test_manifest_without_boxes_is_rejected(tmp_path: Path) -> None:
    bad = tmp_path / "boxes.json"
    bad.write_text(json.dumps({"engine": "e", "config": "c", "output_dir": "o"}))
    with pytest.raises(ValueError, match="at least one box"):
        load_manifest(bad)


def test_boxes_are_auto_named_by_position_and_padded(tmp_path: Path) -> None:
    """Unnamed boxes get zero-padded positional names, sortable in order."""
    doc = {
        "boxes": [
            {"top_left": [i + 1, 0], "bottom_right": [i, 1]} for i in range(12)
        ]
    }
    manifest = tmp_path / "boxes.json"
    manifest.write_text(json.dumps(doc))
    names = [b.name for b in load_manifest(manifest).boxes]
    assert names[0] == "box_000"
    assert names[11] == "box_011"
    # Sorting the names reproduces manifest order (the merge tie-break).
    assert sorted(names) == names


def test_malformed_corner_is_rejected(tmp_path: Path) -> None:
    bad = tmp_path / "boxes.json"
    bad.write_text(
        json.dumps({"boxes": [{"top_left": [1.0], "bottom_right": [0.0, 1.0]}]})
    )
    with pytest.raises(ValueError, match="top_left and bottom_right"):
        load_manifest(bad)


def test_box_bbox_args_are_in_engine_order() -> None:
    box = Box(name="b", north=38.6, west=-123.6, south=37.9, east=-122.3)
    assert box.bbox_args() == ["--bbox", "38.6", "-123.6", "37.9", "-122.3"]


def test_missing_config_via_cli_and_manifest_exits_nonzero(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """No engine/config/output_dir anywhere → a clear config-error exit."""
    manifest = tmp_path / "boxes.json"
    manifest.write_text(
        json.dumps(
            {"boxes": [{"top_left": [1, 0], "bottom_right": [0, 1]}]}
        )
    )
    rc = main([str(manifest)])
    assert rc == 2
    err = capsys.readouterr().err
    assert "engine" in err and "config" in err and "output_dir" in err
