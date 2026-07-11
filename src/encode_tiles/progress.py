"""Optional tqdm progress bars for the CLI.

The library modules stay UI-free: they report progress through plain
``(done, total)`` callbacks, and everything tqdm-shaped lives here.  Bars
degrade to no-ops when tqdm is not installed or stderr is not a terminal,
so redirected/background runs and the test suite emit nothing extra.
"""

from __future__ import annotations

import sys
from typing import Callable, Iterable, Iterator, TypeVar

try:
    from tqdm import tqdm as _tqdm
except ImportError:  # tqdm is a convenience, not a hard requirement
    _tqdm = None

T = TypeVar("T")

# A merge-progress callback: (completed_strips, total_strips).
MergeProgress = Callable[[int, int], None]


def enabled(no_progress: bool) -> bool:
    """True when bars should render: not suppressed, tqdm present, stderr a TTY."""
    return not no_progress and _tqdm is not None and sys.stderr.isatty()


def wrap_rows(
    rows: Iterable[T], total: int, desc: str
) -> Iterable[T]:
    """Wrap the base-zoom tile-row stream in a bar advancing as rows are consumed."""
    return _tqdm(rows, total=total, desc=desc, unit="row", file=sys.stderr)


class MergeBar:
    """A (done, total) callback backed by a lazily-created tqdm bar.

    The bar is built on the first call — when the merged lattice height, and
    thus the strip total, is first known — so single-input encodes (which
    never merge) create no bar at all.
    """

    def __init__(self, desc: str = "merging boxes") -> None:
        self._desc = desc
        self._bar = None

    def __call__(self, done: int, total: int) -> None:
        if self._bar is None:
            self._bar = _tqdm(total=total, desc=self._desc, unit="strip", file=sys.stderr)
        self._bar.update(done - self._bar.n)

    def close(self) -> None:
        if self._bar is not None:
            self._bar.close()
            self._bar = None
