#!/usr/bin/env python3
"""Filter a DEM download list down to the newest tile per quad.

Each line is a USGS 1/3-arc-second GeoTIFF URL whose basename looks like
``USGS_13_<tile>_<YYYYMMDD>.tif`` — e.g. ``USGS_13_n34w117_20260610.tif``.
The ``<tile>`` (``n34w117``) identifies the 1x1 degree quad; the trailing
``<YYYYMMDD>`` is the acquisition date. Many quads appear multiple times with
different acquisition dates. We keep only the newest acquisition per quad so we
download each tile exactly once.

Usage:
    python scripts/dedupe_downloads.py [INPUT] [-o OUTPUT]

Defaults: reads data/downloads.txt, writes deduped list to stdout.
"""

import argparse
import re
import sys
from pathlib import Path

# USGS_13_<tile>_<YYYYMMDD>.tif  ->  capture tile and date
FILENAME_RE = re.compile(r"USGS_13_([a-z]\d+[a-z]\d+)_(\d{8})\.tif", re.IGNORECASE)


def dedupe(lines):
    """Return newest-per-tile URLs, preserving first-seen tile order."""
    newest = {}      # tile -> (date, url)
    order = []       # tiles in the order first encountered
    skipped = []     # lines that didn't match the expected pattern

    for raw in lines:
        url = raw.strip()
        if not url:
            continue
        m = FILENAME_RE.search(url)
        if not m:
            skipped.append(url)
            continue
        tile, date = m.group(1).lower(), m.group(2)
        if tile not in newest:
            order.append(tile)
            newest[tile] = (date, url)
        elif date > newest[tile][0]:      # YYYYMMDD sorts lexicographically
            newest[tile] = (date, url)

    return [newest[t][1] for t in order], skipped


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs="?", default="data/downloads.txt",
                        type=Path, help="input list (default: data/downloads.txt)")
    parser.add_argument("-o", "--output", type=Path,
                        help="write result here instead of stdout")
    args = parser.parse_args(argv)

    lines = args.input.read_text().splitlines()
    kept, skipped = dedupe(lines)

    out = "\n".join(kept) + "\n"
    if args.output:
        args.output.write_text(out)
    else:
        sys.stdout.write(out)

    print(f"{len(lines)} lines in -> {len(kept)} unique tiles kept "
          f"({len(lines) - len(kept) - len(skipped)} duplicates dropped)",
          file=sys.stderr)
    if skipped:
        print(f"warning: {len(skipped)} line(s) did not match the expected "
              f"USGS_13_<tile>_<date>.tif pattern and were dropped",
              file=sys.stderr)


if __name__ == "__main__":
    main()
