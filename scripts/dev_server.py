#!/usr/bin/env python3
"""Static dev server with HTTP Range support.

`python3 -m http.server` does NOT honour Range requests, so it cannot serve the
PMTiles vector basemap (frontend/basemap/basemap.pmtiles) — PMTiles reads the
single file by byte range. Production (S3 + CloudFront) supports byte-serving
natively; this script gives the same locally.

Usage:  python3 scripts/dev_server.py [PORT] [--directory DIR]
Default: port 8092, directory ./frontend
"""
import argparse
import http.server
import os
import re
import sys


class RangeRequestHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler + single-range (bytes=start-end) support."""

    def send_head(self):
        rng = self.headers.get("Range")
        if rng is None:
            return super().send_head()

        m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
        path = self.translate_path(self.path)
        if m is None or not os.path.isfile(path):
            return super().send_head()

        size = os.path.getsize(path)
        start_s, end_s = m.group(1), m.group(2)
        if start_s == "":  # suffix range: last N bytes
            length = min(int(end_s), size)
            start, end = size - length, size - 1
        else:
            start = int(start_s)
            end = int(end_s) if end_s else size - 1
            end = min(end, size - 1)

        if start > end or start >= size:
            self.send_response(416)  # Range Not Satisfiable
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None

        f = open(path, "rb")
        f.seek(start)
        self._range_remaining = end - start + 1
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(self._range_remaining))
        self.end_headers()
        return f

    def end_headers(self):
        # Dev server: never let the browser cache, so edits always show on reload.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def copyfile(self, source, outputfile):
        remaining = getattr(self, "_range_remaining", None)
        if remaining is None:
            return super().copyfile(source, outputfile)
        while remaining > 0:
            chunk = source.read(min(64 * 1024, remaining))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=8092)
    ap.add_argument("--directory", default="frontend")
    args = ap.parse_args()

    os.chdir(args.directory)
    server = http.server.ThreadingHTTPServer(("", args.port), RangeRequestHandler)
    print(f"Range-capable dev server on http://localhost:{args.port}/ "
          f"(serving {args.directory})", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
