"""Progress reporting: the merge callback fires and the CLI flag is accepted."""

from __future__ import annotations

from pathlib import Path

from encode_tiles.cli import build_parser, main
from encode_tiles.mosaic import (
    MERGE_STRIP_HEIGHT,
    compute_base_spec,
    merge_sources,
    open_mosaic_source,
)

from encode_tiles.tests.test_pipeline import B_BOUNDS, ZOOM, _make_fixture


def test_merge_sources_reports_monotonic_strip_progress(tmp_path: Path) -> None:
    box_a = tmp_path / "a.tif"
    box_b = tmp_path / "b.tif"
    _make_fixture(box_a)
    _make_fixture(box_b, bounds=B_BOUNDS)

    calls: list[tuple[int, int]] = []
    merge_sources(
        [box_a, box_b],
        tmp_path / "merged.tif",
        lambda done, total: calls.append((done, total)),
    )

    assert calls, "expected at least one progress callback"
    totals = {total for _, total in calls}
    assert len(totals) == 1, "the strip total must be stable across the merge"
    total = totals.pop()
    # One callback per strip, completed count climbs 1..total and ends full.
    assert [done for done, _ in calls] == list(range(1, total + 1))
    assert calls[-1] == (total, total)


def test_single_input_open_source_never_calls_merge_progress(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    _make_fixture(src_tif)

    calls: list[tuple[int, int]] = []
    with open_mosaic_source(
        [src_tif], lambda done, total: calls.append((done, total))
    ) as (src, contract):
        compute_base_spec(src, contract, zoom=ZOOM)
    assert calls == [], "a single input does not merge, so nothing to report"


def test_no_progress_flag_is_accepted_and_encodes(tmp_path: Path) -> None:
    src_tif = tmp_path / "src.tif"
    out_dir = tmp_path / "tiles"
    _make_fixture(src_tif)

    rc = main(
        [
            "--input", str(src_tif),
            "--output-dir", str(out_dir),
            "--min-zoom", str(ZOOM),
            "--max-zoom", str(ZOOM),
            "--no-progress",
        ]
    )
    assert rc == 0
    assert (out_dir / "tilejson.json").is_file()


def test_no_progress_flag_defaults_false() -> None:
    args = build_parser().parse_args(["--input", "x.tif"])
    assert args.no_progress is False
