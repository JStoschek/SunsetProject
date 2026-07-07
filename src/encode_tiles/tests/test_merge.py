"""Tests for merge_deepest_interior (multi-box overlap arbitration, ADR-0015).

Tiny synthetic arrays stand in for boxes on a common 4326 lattice; assertions
are on the merged output only — which box a pixel came from, that its bytes
are verbatim, that uncovered pixels stay nodata — never on internals.
"""

from __future__ import annotations

import numpy as np
import pytest

from encode_tiles.mosaic import merge_deepest_interior

BPP = 3  # multi-byte pixels so byte-verbatim copying is actually exercised


def _box(height: int, width: int, fill: bytes) -> np.ndarray:
    """A box whose every pixel carries the same distinctive byte pattern."""
    assert len(fill) == BPP
    return np.tile(np.frombuffer(fill, dtype=np.uint8), (height, width, 1))


A_BYTES = b"\xaa\x01\x80"
B_BYTES = b"\xbb\x02\x40"


def test_north_south_overlap_splits_at_midline() -> None:
    """Two stacked boxes: each owns the overlap half nearer its own centre."""
    # A rows 0–9, B rows 6–15 on the lattice → overlap rows 6–9, midline
    # between rows 7 and 8.
    a = _box(10, 8, A_BYTES)
    b = _box(10, 8, B_BYTES)
    merged = merge_deepest_interior([a, b], [(0, 0), (6, 0)], 16, 8)

    col = 2  # interior column: the east-edge term must not bind here
    assert bytes(merged[6, col]) == A_BYTES
    assert bytes(merged[7, col]) == A_BYTES
    assert bytes(merged[8, col]) == B_BYTES
    assert bytes(merged[9, col]) == B_BYTES


def test_crossover_is_a_straight_line_across_columns() -> None:
    """The seam sits at the same row for every interior column — no staircase.

    Columns within the seam-depth of the shared east edge are excluded: there
    both boxes' east-edge terms bind identically, so they tie and the lowest
    index wins — geometry no longer separates them.
    """
    a = _box(10, 8, A_BYTES)
    b = _box(10, 8, B_BYTES)
    merged = merge_deepest_interior([a, b], [(0, 0), (6, 0)], 16, 8)

    for col in range(6):
        for row in range(6, 8):
            assert bytes(merged[row, col]) == A_BYTES, f"row {row}, col {col}"
        for row in range(8, 10):
            assert bytes(merged[row, col]) == B_BYTES, f"row {row}, col {col}"


def test_west_edge_adjacent_pixel_keeps_its_boxs_verdict() -> None:
    """A pixel hugging its box's west (offshore) edge is never down-weighted.

    The pixel at lattice (4, 0) is half a cell from A's west edge but 4.5
    cells from A's north/south edges.  B covers it only 2.5 cells from B's
    north edge.  Were the west edge counted, A would score 0.5 and lose;
    with west excluded A scores 4.5 and must win.
    """
    a = _box(9, 20, A_BYTES)
    b = _box(20, 20, B_BYTES)
    merged = merge_deepest_interior([a, b], [(0, 0), (2, 0)], 22, 20)

    assert bytes(merged[4, 0]) == A_BYTES


def test_winning_bytes_are_verbatim() -> None:
    """Merged pixels are byte-identical to one source — bits never blend."""
    rng = np.random.default_rng(42)
    a = rng.integers(0, 256, size=(6, 6, BPP), dtype=np.uint8)
    b = rng.integers(0, 256, size=(6, 6, BPP), dtype=np.uint8)
    merged = merge_deepest_interior([a, b], [(0, 0), (3, 0)], 9, 6)

    for row in range(9):
        for col in range(6):
            pixel = bytes(merged[row, col])
            in_a = row < 6 and pixel == bytes(a[row, col])
            in_b = row >= 3 and pixel == bytes(b[row - 3, col])
            assert in_a or in_b, f"pixel ({row},{col}) matches neither source"


def test_exact_tie_breaks_to_lowest_source_index() -> None:
    """Coincident boxes tie everywhere; source 0 must win every pixel."""
    a = _box(6, 6, A_BYTES)
    b = _box(6, 6, B_BYTES)
    merged = merge_deepest_interior([a, b], [(0, 0), (0, 0)], 6, 6)

    assert (merged == np.frombuffer(A_BYTES, dtype=np.uint8)).all()

    # Deterministic under re-runs by construction, but assert it anyway.
    again = merge_deepest_interior([a, b], [(0, 0), (0, 0)], 6, 6)
    assert (merged == again).all()


def test_uncovered_pixels_are_nodata() -> None:
    """Disjoint boxes with a gap: gap pixels stay all-zero (transparent)."""
    a = _box(4, 4, A_BYTES)
    b = _box(4, 4, B_BYTES)
    merged = merge_deepest_interior([a, b], [(0, 0), (10, 0)], 14, 4)

    assert (merged[4:10, :, :] == 0).all()
    assert bytes(merged[0, 0]) == A_BYTES
    assert bytes(merged[13, 0]) == B_BYTES


def test_nested_box_defers_to_the_larger_boxs_deeper_interior() -> None:
    """A box fully contained in a much larger box contributes nothing."""
    small = _box(4, 4, B_BYTES)
    large = _box(20, 20, A_BYTES)
    # Small sits well inside large, away from all of large's edges.
    merged = merge_deepest_interior([large, small], [(0, 0), (8, 8)], 20, 20)

    assert (merged == np.frombuffer(A_BYTES, dtype=np.uint8)).all()


def test_placement_outside_lattice_is_rejected() -> None:
    a = _box(4, 4, A_BYTES)
    with pytest.raises(ValueError):
        merge_deepest_interior([a], [(2, 0)], 4, 4)
