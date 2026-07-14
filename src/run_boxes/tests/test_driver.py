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


# --- Incremental runs: skip-if-fresh, --force, and pruning (ADR-0017) -------


def _setup(tmp_path: Path, exit_code: int = 0) -> tuple[Path, Path, Path]:
    """A ready-to-run engine + config + two-box manifest. Returns them."""
    engine = tmp_path / "engine.py"
    log = _fake_engine(engine, exit_code=exit_code)
    config = tmp_path / "pipeline.conf"
    config.write_text("stub config")
    out_dir = tmp_path / "boxes"
    manifest = _manifest(tmp_path / "boxes.json", engine, config, out_dir)
    return manifest, log, out_dir


def test_unchanged_boxes_are_skipped_on_second_run(tmp_path: Path) -> None:
    """A second run with no manifest/config change runs the engine zero times."""
    manifest, log, out_dir = _setup(tmp_path)

    assert main([str(manifest)]) == 0
    assert len(log.read_text().splitlines()) == 2  # both boxes ran

    # Sidecars were written next to each tif, on success.
    assert (out_dir / "box_000.tif.json").is_file()
    assert (out_dir / "box_001.tif.json").is_file()

    log.write_text("")  # reset the call log
    assert main([str(manifest)]) == 0
    assert log.read_text() == ""  # nothing re-ran — both skipped


def test_changed_bounds_reprocess_only_that_box(tmp_path: Path) -> None:
    manifest, log, out_dir = _setup(tmp_path)
    assert main([str(manifest)]) == 0

    # Nudge box 1's bounds; box 0 is untouched.
    doc = json.loads(manifest.read_text())
    doc["boxes"][1]["top_left"] = [38.09, -123.20]
    manifest.write_text(json.dumps(doc))

    log.write_text("")
    assert main([str(manifest)]) == 0
    calls = log.read_text().splitlines()
    assert len(calls) == 1  # only the changed box re-ran
    assert "--bbox 38.09 -123.2 37.3 -121.79" in calls[0]


def test_changed_config_reprocesses_all_boxes(tmp_path: Path) -> None:
    manifest, log, out_dir = _setup(tmp_path)
    assert main([str(manifest)]) == 0

    # A config edit changes the hash, invalidating every box.
    (tmp_path / "pipeline.conf").write_text("stub config -- tweaked")

    log.write_text("")
    assert main([str(manifest)]) == 0
    assert len(log.read_text().splitlines()) == 2


def test_force_reprocesses_unchanged_boxes(tmp_path: Path) -> None:
    manifest, log, out_dir = _setup(tmp_path)
    assert main([str(manifest)]) == 0

    log.write_text("")
    assert main([str(manifest), "--force"]) == 0
    assert len(log.read_text().splitlines()) == 2  # --force ignored freshness


def test_crash_writes_no_sidecar_so_box_reruns(tmp_path: Path) -> None:
    """A failed engine leaves no sidecar; the box is not treated as done."""
    manifest, log, out_dir = _setup(tmp_path, exit_code=1)

    assert main([str(manifest)]) != 0
    # The engine touched box_000.tif before exiting nonzero, but no sidecar.
    assert not (out_dir / "box_000.tif.json").exists()

    # Repoint at a succeeding engine; the box must re-run, not skip.
    good = tmp_path / "engine.py"
    good_log = _fake_engine(good, exit_code=0)
    doc = json.loads(manifest.read_text())
    doc["engine"] = str(good)
    manifest.write_text(json.dumps(doc))

    good_log.write_text("")  # shared log path — reset before the good run
    assert main([str(manifest)]) == 0
    assert len(good_log.read_text().splitlines()) == 2  # both boxes ran


def test_missing_sidecar_reprocesses_preexisting_tif(tmp_path: Path) -> None:
    """A tif with no sidecar (pre-feature output) reprocesses once."""
    manifest, log, out_dir = _setup(tmp_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    # Simulate a leftover tif from before this feature: tif but no sidecar.
    (out_dir / "box_000.tif").write_bytes(b"old")

    assert main([str(manifest)]) == 0
    # box_000 had no matching sidecar → it still ran (both boxes ran).
    assert len(log.read_text().splitlines()) == 2


def test_shorter_manifest_prunes_trailing_orphans(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    manifest, log, out_dir = _setup(tmp_path)
    assert main([str(manifest)]) == 0
    assert (out_dir / "box_001.tif").is_file()
    assert (out_dir / "box_001.tif.json").is_file()

    # Drop the manifest to a single box; box_001's tif + sidecar are orphans.
    doc = json.loads(manifest.read_text())
    doc["boxes"] = doc["boxes"][:1]
    manifest.write_text(json.dumps(doc))

    capsys.readouterr()  # clear
    assert main([str(manifest)]) == 0
    assert not (out_dir / "box_001.tif").exists()
    assert not (out_dir / "box_001.tif.json").exists()
    assert (out_dir / "box_000.tif").is_file()  # surviving box untouched
    assert "Pruned" in capsys.readouterr().out


# --- Fill-polygon regeneration: the fill is derived from the manifest -------


def _fake_fill_script(path: Path, exit_code: int = 0) -> Path:
    """Write a python script that logs its argv, standing in for the builder."""
    log = path.parent / "fill_calls.log"
    script = f"""#!/usr/bin/env python3
import sys
with open({str(log)!r}, "a") as f:
    f.write(" ".join(sys.argv[1:]) + "\\n")
sys.exit({exit_code})
"""
    path.write_text(script)
    return log


def test_fill_regenerated_after_every_successful_pass(tmp_path: Path) -> None:
    """With fill_dir set, the builder runs even on an all-skipped pass.

    The fill's own --if-stale check is what makes the no-change case cheap;
    the driver must not second-guess it, or a boxes edit that only touches
    the fill (today's bug) would be missed.
    """
    manifest, log, out_dir = _setup(tmp_path)
    doc = json.loads(manifest.read_text())
    doc["fill_dir"] = str(tmp_path / "frontend")
    manifest.write_text(json.dumps(doc))
    fill_script = tmp_path / "fill_script.py"
    fill_log = _fake_fill_script(fill_script)

    assert main([str(manifest), "--fill-script", str(fill_script)]) == 0
    assert main([str(manifest), "--fill-script", str(fill_script)]) == 0  # all-skip pass

    calls = fill_log.read_text().splitlines()
    assert len(calls) == 2  # invoked on both passes
    for call in calls:
        assert f"--boxes {manifest}" in call
        assert f"--out-dir {tmp_path / 'frontend'}" in call
        assert "--if-stale" in call


def test_fill_failure_fails_the_run(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A failed fill rebuild must not exit 0 — a stale fill breaks the map."""
    manifest, log, out_dir = _setup(tmp_path)
    doc = json.loads(manifest.read_text())
    doc["fill_dir"] = str(tmp_path / "frontend")
    manifest.write_text(json.dumps(doc))
    fill_script = tmp_path / "fill_script.py"
    _fake_fill_script(fill_script, exit_code=1)

    rc = main([str(manifest), "--fill-script", str(fill_script)])
    assert rc != 0
    assert "us_land_fill" in capsys.readouterr().err


def test_no_fill_dir_skips_regeneration(tmp_path: Path) -> None:
    """Without fill_dir (manifest or CLI), the builder is never invoked."""
    manifest, log, out_dir = _setup(tmp_path)
    fill_script = tmp_path / "fill_script.py"
    fill_log_path = tmp_path / "fill_calls.log"
    _fake_fill_script(fill_script)

    assert main([str(manifest), "--fill-script", str(fill_script)]) == 0
    assert not fill_log_path.exists()


def test_engine_failure_prunes_nothing(tmp_path: Path) -> None:
    """A mid-run crash stops early and must not prune trailing outputs."""
    # First, a clean two-box run so box_001 exists.
    manifest, log, out_dir = _setup(tmp_path)
    assert main([str(manifest)]) == 0
    assert (out_dir / "box_001.tif").is_file()

    # Shrink the manifest, point at a failing engine, AND change the surviving
    # box's bounds so it actually re-runs (and fails) rather than skipping.
    bad = tmp_path / "bad_engine.py"
    _fake_engine(bad, exit_code=1)
    doc = json.loads(manifest.read_text())
    doc["boxes"] = doc["boxes"][:1]
    doc["boxes"][0]["top_left"] = [38.61, -123.60]
    doc["engine"] = str(bad)
    manifest.write_text(json.dumps(doc))

    assert main([str(manifest)]) != 0
    # The run failed before completing, so the orphan must survive.
    assert (out_dir / "box_001.tif").is_file()
