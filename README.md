# Sunset Scout

**Live site: <https://sunsetscout.com/> — how it works: <https://sunsetscout.com/how-it-works.html>**

A map of where you can actually see the sun touch the Pacific horizon, for any date of
the year.

Pick a date; the map paints every land pixel white (the sunset is visible from there) or
black (terrain gets in the way). The verdict is real line-of-sight geometry against a
10-metre USGS elevation model — not a guess from distance to the coast. Hills, headlands
and the curvature of the earth are all in it; buildings and trees are not.

---

## How it works, in one pass

The heavy geometry runs **offline**, in C++. The browser only decides what to colour.

1. **Sweep.** For each sunset azimuth from 233° to 309° in 1° steps, rays are cast inland
   from the open Pacific. Each ray finds its **coastline crossing**, then marches east
   accumulating the running maximum **horizon reach** — `√(h/c) − x`, how far seaward a
   point of height `h` can see, with earth curvature and atmospheric refraction folded
   into `c`. An observer (eyes 2 m up) sees the sunset iff their own reach meets that
   running maximum. The test is a horizon-tangent comparison, not a sighting on a fixed
   reference point, and it costs O(n) per ray.
2. **Stack.** Each azimuth contributes one bit per pixel. The result is a per-pixel
   **Visible Azimuth Set** — possibly with gaps, since a near ridge and a far one can
   leave a pixel visible, blocked, then visible again.
3. **Encode.** The bitmasks become XYZ tiles (`/{z}/{x}/{y}.bin`) with a sidecar
   TileJSON that describes the azimuth window, step and bit count. The wire format is
   self-describing — the frontend hard-codes none of it.
4. **Colourize.** MapLibre GL JS loads the tiles, SunCalc computes the sunset azimuth for
   the chosen date and map position, and the shader picks white/black/transparent
   per pixel. Scrubbing the date bar is a pure client-side recolour: no refetch, no
   round-trip.

## Repository layout

| Path | What's in it |
|---|---|
| `src/horizon_sweep_engine/` | The sweep itself — one azimuth slice per call (C++) |
| `src/dem_tile_loader/` | On-demand elevation from 1°×1° USGS GeoTIFFs, LRU-cached |
| `src/ocean_mask_rasterizer/` | OSM water polygons → 1/3″ land/water bits; coastline crossings |
| `src/compute_azimuth_range/` | Driver: one bounding box → one multi-band GeoTIFF |
| `src/run_boxes/` | Manifest-driven multi-box runs with freshness sidecars (Python) |
| `src/encode_tiles/` | GeoTIFF(s) → merged tile pyramid + TileJSON (Python) |
| `src/trace_ray/` | Debug tool: dump a single ray's samples as plain yes/no points |
| `src/bit_layout/`, `src/geo/` | Shared bit-packing contract and tile identity |
| `frontend/` | The map: MapLibre, a local Protomaps basemap, colourizer, date bar |
| `config/pipeline.conf` | Every tunable. Nothing physical is hard-coded in the C++ |
| `scripts/deploy.sh` | S3 + CloudFront deploy with the right per-asset cache headers |

## Getting set up

**Requirements:** CMake ≥ 3.20 and a C++17 compiler, GDAL (`brew install gdal`), Python 3
with `pytest`, Node (for the frontend tests and SunCalc).

```bash
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
npm install
```

Then fetch the two external datasets and point `config/pipeline.conf` at them:

- **Elevation** — USGS 1/3-arc-second DEM, as 1°×1° GeoTIFFs (`USGS_13_n{n}w{w}.tif`)
  from the National Map, into `data/DEM/` (`dem_dir`).
- **Water** — the osmdata.openstreetmap.de full-resolution *split* `water-polygons`
  shapefile in EPSG:4326, into `data/OSM/` (`osm_water_polygons_path`). This is the ocean
  and coastline; inland lakes are deliberately left out and treated as land.

Both are large downloads and live outside the repo. Everything else about the run — eye
height, refraction, sample spacing, the azimuth window, strip size, thread count — is a
key in `config/pipeline.conf`, changeable without recompiling.

## Running the pipeline

Coverage is a union of **Processing Boxes**, each anchored with its western edge offshore
so every ray can seed in water and march east to land. List them in a `boxes.json`
(start from [`boxes.example.json`](boxes.example.json)), then:

```bash
python3 -m run_boxes boxes.json
```

```bash
python3 -m encode_tiles --input build/boxes --output-dir frontend/tiles/
```

`run_boxes` skips boxes whose bounds and config hash still match their provenance
sidecar, so re-runs are incremental (`--force` to override). Overlapping boxes are
merged by deepest interior, which hides the seams.

Serve it locally — the basemap is a single `.pmtiles` read by HTTP **byte range**, which
`python3 -m http.server` does not support:

```bash
python3 scripts/dev_server.py 8080 --directory frontend
```

Box editor at `http://localhost:8080/boxes.html`: draw, drag and save boxes straight into
`boxes.json`. More recipes in [`QUICKSTART.md`](QUICKSTART.md).

## Tests

```bash
cmake --build build-release --target test
```

```bash
pytest src/encode_tiles/tests/ src/run_boxes/tests/ && npm run test:frontend
```

The bit-packing layout is implemented twice — `src/bit_layout/BitLayout.h` and
`frontend/bit_layout.js` — so both sides are pinned against the shared
`src/bit_layout/vectors.json` fixture; change one without the other and the tests say so.
The
`CoastalRegression` test guards the coast crossing specifically: visibility is fragile
there, and a past optimization silently blacked out Ocean Beach.

## Deploying

`scripts/deploy.sh` handles it — correct `Content-Type` and `Cache-Control` per asset
class, plus targeted CloudFront invalidation. Code and TileJSON get a 5-minute TTL; tiles
and the basemap are immutable and invalidated explicitly when they change. One gotcha it
protects you from: `tiles/*.bin` are already gzipped on disk and must **not** carry
`Content-Encoding: gzip`, or the browser double-decodes and the overlay breaks.

## Known limits

- **Bare earth only.** No buildings, no trees. A downtown window may say "visible".
- **Coastline disagreement.** The DEM (USGS) and the basemap coastline (OSM) differ by
  up to ~100 m in places like Big Sur. It's structural, not a registration bug.
- **Sunset time ≠ sunset visible.** The info panel shows almanac times for any point;
  those are astronomy, independent of whether terrain lets you see it.
- **Coverage is US, Alaska excluded**, and only where boxes have been run.
