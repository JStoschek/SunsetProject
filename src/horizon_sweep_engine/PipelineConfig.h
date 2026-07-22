#pragma once
#include <string>

/// All tunable parameters for the offline processing pipeline, loaded from a
/// `key = value` config file (config/pipeline.conf). No physical constant or
/// step size is hard-coded in the engine; every knob lives here so it can be
/// tuned without recompiling. Per-run inputs (the bounding box) are NOT here —
/// they are passed per invocation.
struct PipelineConfig {
    // ── Physical constants ──────────────────────────────────────────────
    double refraction_coefficient_k;   ///< atmospheric refraction coefficient k
    double earth_radius_m;             ///< mean Earth radius R (metres)
    double observer_eye_height_m;      ///< observer eye height above bare ground

    // ── Line-of-sight model (ADR-0008 / ADR-0016) ───────────────────────
    double coast_obstruction_skip_m;   ///< terrain within this many metres inland of the
                                       ///< coastline crossing does not raise the running
                                       ///< max Horizon Reach (passable foreshore:
                                       ///< foredunes/berms don't shadow their own lee).
                                       ///< 0 = disabled.

    // ── Grid ────────────────────────────────────────────────────────────
    double cell_per_degree;            ///< cells per degree (1/3 arc-second = 10800)
    double meters_per_degree_lat;      ///< flat-earth metres per degree of latitude
    double sample_spacing_arcsec;      ///< Visibility Sample Grid spacing (ADR-0014):
                                       ///< drives BOTH the perp ray spacing and the
                                       ///< along march step (square in the ray frame).
                                       ///< Lower = sharper visibility boundaries;
                                       ///< cost scales with 1/s² (halving the spacing
                                       ///< roughly quadruples the sweep work).

    // ── Azimuth sweep ───────────────────────────────────────────────────
    double azimuth_min_deg;            ///< inclusive lower sunset azimuth
    double azimuth_max_deg;            ///< inclusive upper sunset azimuth
    double azimuth_step_deg;           ///< azimuth step between slices

    // ── Azimuth range pipeline ──────────────────────────────────────────
    double strip_height_deg;           ///< latitude height of each processing strip
    double strip_tilt_margin_deg;      ///< lat margin added N/S to a strip's frozen
                                       ///< working set for the ray tilt (ADR-0007)

    // ── Coast finding ───────────────────────────────────────────────────
    double coast_march_max_km;         ///< per-ray give-up distance for the in-march
                                       ///< coast search (ADR-0014); a ray with no
                                       ///< coast within this range yields no
                                       ///< visible samples

    // ── West-edge preflight (ADR-0015) ──────────────────────────────────
    double west_edge_max_land_frac;    ///< tolerated fraction of land samples on the
                                       ///< box's western-edge column before the run
                                       ///< hard-errors as mis-anchored (ADR-0008)

    // ── Parallelism ──────────────────────────────────────────────────────
    int    worker_threads;             ///< pool size; 0 = std::thread::hardware_concurrency()

    // ── Tile caches ─────────────────────────────────────────────────────
    int    dem_lru_capacity;           ///< DEM tiles held resident
    int    ocean_lru_capacity;         ///< ocean-mask tiles held resident

    // ── Data paths ───────────────────────────────────────────────────
    std::string dem_dir;               ///< directory containing USGS GeoTIFF tiles
    std::string osm_water_polygons_path; ///< osmdata split `water-polygons` shapefile
                                         ///< (ocean + coastline); the Ocean Mask source
    std::string osm_inland_water_path;   ///< Geofabrik `natural=water` extract (inland
                                         ///< lakes/ponds); empty to omit inland water

    /// Parse a pipeline config file. Lines are `key = value`; `#` begins a
    /// comment; blank lines are ignored. Throws std::runtime_error if the file
    /// cannot be opened or a required key is missing.
    static PipelineConfig load(const std::string& path);
};
