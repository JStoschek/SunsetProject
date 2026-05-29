"""Tests for the encode_tiles CLI scaffold."""

from __future__ import annotations

from pathlib import Path

import pytest

from encode_tiles.cli import (
    DEFAULT_MAX_ZOOM,
    DEFAULT_MIN_ZOOM,
    DEFAULT_OUTPUT_DIR,
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
