"""Tests for the encode_tiles CLI scaffold."""

from __future__ import annotations

from pathlib import Path

import pytest

from encode_tiles.cli import (
    DEFAULT_MAX_ZOOM,
    DEFAULT_MIN_ZOOM,
    DEFAULT_OUTPUT_DIR,
    _expand_inputs,
    build_parser,
    main,
)


def test_no_args_prints_usage_and_exits_nonzero(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        main([])
    assert exc_info.value.code != 0
    err = capsys.readouterr().err
    assert "usage:" in err.lower()


def test_missing_input_exits_nonzero(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    rc = main(["--input", str(tmp_path / "nope.tif")])
    assert rc != 0
    assert "does not exist" in capsys.readouterr().err


def test_min_zoom_greater_than_max_zoom_exits_nonzero(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    tif = tmp_path / "in.tif"
    tif.write_bytes(b"")
    rc = main(["--input", str(tif), "--min-zoom", "10", "--max-zoom", "5"])
    assert rc != 0
    assert "min-zoom" in capsys.readouterr().err


def test_defaults() -> None:
    parser = build_parser()
    args = parser.parse_args(["--input", "x.tif"])
    assert args.output_dir == DEFAULT_OUTPUT_DIR
    assert args.min_zoom == DEFAULT_MIN_ZOOM == 6
    assert args.max_zoom == DEFAULT_MAX_ZOOM == 14


def test_expand_inputs_globs_folder_sorted(tmp_path: Path) -> None:
    (tmp_path / "b.tif").write_bytes(b"")
    (tmp_path / "a.tif").write_bytes(b"")
    (tmp_path / "notes.txt").write_bytes(b"")  # ignored
    resolved = _expand_inputs([tmp_path])
    assert resolved == [tmp_path / "a.tif", tmp_path / "b.tif"]


def test_expand_inputs_mixes_files_and_folders(tmp_path: Path) -> None:
    folder = tmp_path / "boxes"
    folder.mkdir()
    (folder / "north.tif").write_bytes(b"")
    solo = tmp_path / "solo.tif"
    solo.write_bytes(b"")
    resolved = _expand_inputs([solo, folder])
    assert resolved == [solo, folder / "north.tif"]


def test_expand_inputs_empty_folder_raises(tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(ValueError, match="no .tif files"):
        _expand_inputs([empty])


def test_empty_input_folder_exits_nonzero(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    empty = tmp_path / "empty"
    empty.mkdir()
    rc = main(["--input", str(empty)])
    assert rc == 2
    assert "no .tif files" in capsys.readouterr().err
