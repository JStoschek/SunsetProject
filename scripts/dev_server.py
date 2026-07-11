#!/usr/bin/env python3
"""Static dev server with HTTP Range support.

`python3 -m http.server` does NOT honour Range requests, so it cannot serve the
PMTiles vector basemap (frontend/basemap/basemap.pmtiles) — PMTiles reads the
single file by byte range. Production (S3 + CloudFront) supports byte-serving
natively; this script gives the same locally.

Also exposes the Processing Box manifest for the box editor (boxes.html):
GET /boxes.json returns the repo-root boxes.json; POST /boxes.json replaces
ONLY its "boxes" array (all other keys round-trip verbatim). That one file is
the only thing this server can write — every other path stays read-only.

Usage:  python3 scripts/dev_server.py [PORT] [--directory DIR]
Default: port 8092, directory ./frontend
"""
import argparse
import http.server
import json
import os
import re
import sys

# Absolute path of the ONE file the server may write (repo-root boxes.json),
# resolved at startup before the chdir into the served directory.
BOXES_PATH = None
# Fallback template when boxes.json does not exist yet.
BOXES_EXAMPLE_PATH = None
BOXES_ROUTE = "/boxes.json"


def _load_manifest_template():
    """Current boxes.json, else boxes.example.json, else minimal defaults."""
    for path in (BOXES_PATH, BOXES_EXAMPLE_PATH):
        if path and os.path.isfile(path):
            with open(path) as f:
                doc = json.load(f)
            if isinstance(doc, dict):
                return doc
    return {
        "engine": "build-release/src/compute_azimuth_range/compute_azimuth_range",
        "config": "config/pipeline.conf",
        "output_dir": "build/boxes",
        "boxes": [],
    }


def _validate_boxes(raw):
    """Check a posted boxes array; return an error string or None."""
    if not isinstance(raw, list):
        return "'boxes' must be an array"
    for i, entry in enumerate(raw):
        if not isinstance(entry, dict) or set(entry) != {"top_left", "bottom_right"}:
            return f"box {i} must have exactly top_left and bottom_right"
        for key in ("top_left", "bottom_right"):
            val = entry[key]
            if (not isinstance(val, list) or len(val) != 2
                    or not all(isinstance(c, (int, float)) and not isinstance(c, bool)
                               and c == c for c in val)):
                return f"box {i} {key} must be a [lat, lon] pair of numbers"
    return None


class RangeRequestHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler + single-range (bytes=start-end) support."""

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.split("?")[0] == BOXES_ROUTE:
            if not os.path.isfile(BOXES_PATH):
                self._send_json(404, {"error": "boxes.json does not exist yet"})
                return
            with open(BOXES_PATH, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        # The manifest route is the ONLY writable path on this server.
        if self.path.split("?")[0] != BOXES_ROUTE:
            self._send_json(405, {"error": "read-only server; only POST /boxes.json"})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            doc = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError) as exc:
            self._send_json(400, {"error": f"invalid JSON body: {exc}"})
            return
        if not isinstance(doc, dict) or "boxes" not in doc:
            self._send_json(400, {"error": "body must be an object with a 'boxes' key"})
            return
        err = _validate_boxes(doc["boxes"])
        if err is not None:
            self._send_json(400, {"error": err})
            return

        # Replace only the boxes array; every other manifest key round-trips.
        try:
            manifest = _load_manifest_template()
        except (OSError, json.JSONDecodeError) as exc:
            self._send_json(500, {"error": f"could not read existing manifest: {exc}"})
            return
        manifest["boxes"] = doc["boxes"]

        tmp = BOXES_PATH + ".tmp"
        try:
            with open(tmp, "w") as f:
                json.dump(manifest, f, indent=2)
                f.write("\n")
            os.replace(tmp, BOXES_PATH)  # atomic: no torn boxes.json on crash
        except OSError as exc:
            self._send_json(500, {"error": f"write failed: {exc}"})
            return
        self._send_json(200, {"ok": True, "boxes": len(manifest["boxes"])})

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
    global BOXES_PATH, BOXES_EXAMPLE_PATH
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=8092)
    ap.add_argument("--directory", default="frontend")
    ap.add_argument("--boxes", default="boxes.json",
                    help="Processing Box manifest served/written at /boxes.json")
    args = ap.parse_args()

    # Resolve the manifest against the LAUNCH cwd (usually the repo root),
    # before chdir moves us into the served directory.
    BOXES_PATH = os.path.abspath(args.boxes)
    BOXES_EXAMPLE_PATH = os.path.join(os.path.dirname(BOXES_PATH), "boxes.example.json")

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
