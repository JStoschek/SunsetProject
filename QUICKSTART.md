# SunsetProject — Quick Commands

## Building

### C++ Pipeline (first time setup)
```bash
cmake -B build -S .
cmake --build build
```

### C++ Pipeline (rebuild)
```bash
cmake --build build
```

### C++ with Release optimization
```bash
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

### C++ with Debug symbols & ASAN
```bash
cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON
cmake --build build-asan
```

## Testing

### Run C++ tests
```bash
cmake --build build --target test
```

### Run Python tests (encode_tiles)
```bash
pytest src/encode_tiles/tests/
```

### Run frontend tests
```bash
npm test:frontend
```

## Frontend

### Install dependencies
```bash
npm install
```

### Serve frontend locally
```bash
# Simple HTTP server (Python 3)
python3 -m http.server 8080

# Or use Node/npm (requires http-server package)
npx http-server frontend -p 8080
```

Open browser to: `http://localhost:8080/frontend/`

## Pipeline Tools

### Encode the Visible Azimuth Set to tiles
```bash
python3 -m encode_tiles --input <geotiff-path> --output-dir <output-dir>
```

### Run full pipeline
```bash
# Compute the per-pixel Visible Azimuth Set
./build/src/compute_azimuth_range/compute_azimuth_range

# Encode results to tiles
python3 -m encode_tiles --input azimuth_range.tif --output-dir frontend/tiles/
```

### Multi-box coverage (ADR-0015)
Draw several Processing Boxes, run the engine once per box, then merge them
all into ONE tileset. Boxes stack north–south along the coast and should
**deliberately overlap** (a generous latitude band) so the deepest-interior
merge splits the seam cleanly down the middle. Every box's western edge must
be offshore (ADR-0008); the engine hard-errors on a mis-anchored box before
writing anything (tolerance knob: `west_edge_max_land_frac` in
`config/pipeline.conf`).

**Recommended: manifest-driven.** List the boxes in a `boxes.json` and let the
driver run the engine per box into a folder; then point `encode_tiles` at the
folder.
Each box is two corners, `[lat, lon]` each: the north-west `top_left` and the
south-east `bottom_right`. Boxes are unnamed — the driver names them by
position (`box_000.tif`, `box_001.tif`, …).
```json
{
  "engine": "build-release/src/compute_azimuth_range/compute_azimuth_range",
  "config": "config/pipeline.conf",
  "output_dir": "build/boxes",
  "boxes": [
    {"top_left": [38.60, -123.60], "bottom_right": [37.90, -122.30]},
    {"top_left": [38.08, -123.20], "bottom_right": [37.30, -121.79]}
  ]
}
```
```bash
# Run the engine once per box → build/boxes/box_NNN.tif
python3 -m run_boxes boxes.json

# Merge the whole folder → one pyramid + one tilejson.json
python3 -m encode_tiles --input build/boxes --output-dir frontend/tiles/
```
`run_boxes` stops on the first box the engine rejects (e.g. a mis-anchored
box), naming it. `engine` / `config` / `output_dir` can also be passed as
`--engine` / `--config` / `--output-dir` (CLI overrides the manifest).

**Manual alternative.** `--input` is repeatable, so you can list boxes
explicitly instead of using a folder:
```bash
./build-release/src/compute_azimuth_range/compute_azimuth_range \
  --config config/pipeline.conf --bbox 38.60 -123.60 37.90 -122.30 \
  --output box_north.tif
./build-release/src/compute_azimuth_range/compute_azimuth_range \
  --config config/pipeline.conf --bbox 38.08 -123.20 37.30 -121.79 \
  --output box_south.tif
python3 -m encode_tiles \
  --input box_north.tif --input box_south.tif --output-dir frontend/tiles/
```
All inputs must share the azimuth window/step, bit count, and format version
(i.e. be produced with one `pipeline.conf`) — `encode_tiles` refuses to merge
mismatched contracts. Disjoint boxes are fine: the gap stays transparent.

## Clean

### Remove build artifacts
```bash
rm -rf build build-debug build-release build-asan
```

### Remove Python cache
```bash
find . -type d -name __pycache__ -exec rm -rf {} +
find . -name "*.pyc" -delete
```
cmake --build build-release

./build-release/src/compute_azimuth_range/compute_azimuth_range \
  --config config/pipeline.conf \
  --bbox 38.075699804065565 -123.20240716979919 37.30303188098905 -121.79394046484315 \
  --output azimuth_range.tif

python3 -m encode_tiles --input azimuth_range.tif --output-dir frontend/tiles/

python3 -m http.server 8080

```bash
cmake --build build-release && \
./build-release/src/compute_azimuth_range/compute_azimuth_range \
  --config config/pipeline.conf \
  --bbox 38.075699804065565 -123.20240716979919 37.30303188098905 -121.79394046484315 \
  --output azimuth_range.tif && \
python3 -m encode_tiles --input azimuth_range.tif --output-dir frontend/tiles/ && \
python3 -m http.server 8080
```

```bash
cmake --build build-release && \
./build-release/src/trace_ray/trace_ray \
  --config config/pipeline.conf \
  --lat 37.5986 --lon -122.5151 --azimuth 301 \
  --bbox 38.075699804065565 -123.20240716979919 37.30303188098905 -121.79394046484315 \
  --before 25 --after 70
```


```bash
cmake --build build-release && \
python3 -m run_boxes boxes.json && \
python3 -m encode_tiles --input build/boxes --output-dir frontend/tiles/ && \
python3 -m http.server 8080
```
