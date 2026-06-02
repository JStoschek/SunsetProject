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

### Encode azimuth range to tiles
```bash
python3 -m encode_tiles <geotiff-path> <output-dir>
```

### Run full pipeline
```bash
# Process azimuth range computation
./build/src/compute_azimuth_range/compute_azimuth_range

# Encode results to tiles
python3 -m encode_tiles azimuth_range.tif frontend/tiles/
```

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
./build-release/src/compute_azimuth_range/compute_azimuth_range \
  --config config/pipeline.conf \
  --bbox 38.075699804065565 -123.20240716979919 37.30303188098905 -121.79394046484315 \
  --output azimuth_range.tif

python3 -m encode_tiles --input azimuth_range.tif --output-dir frontend/tiles/

python3 -m http.server 8080
