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

    // ── Line-of-sight model (ADR-0008) ──────────────────────────────────
    double horizon_reference_offset_m; ///< seaward offset of the Horizon Reference; d at the coast

    // ── Grid ────────────────────────────────────────────────────────────
    double cell_per_degree;            ///< cells per degree (1/3 arc-second = 10800)
    double meters_per_degree_lat;      ///< flat-earth metres per degree of latitude
    double march_step_m;               ///< Phase-1 along-ray sample step (metres)

    // ── Azimuth sweep ───────────────────────────────────────────────────
    double azimuth_min_deg;            ///< inclusive lower sunset azimuth
    double azimuth_max_deg;            ///< inclusive upper sunset azimuth
    double azimuth_step_deg;           ///< azimuth step between slices

    // ── Azimuth range pipeline ──────────────────────────────────────────
    double strip_height_deg;           ///< latitude height of each processing strip

    // ── Coast finding ───────────────────────────────────────────────────
    double coast_march_step_km;        ///< eastward is_water march step
    double coast_march_max_km;         ///< give-up distance for the coast search

    // ── Tile caches ─────────────────────────────────────────────────────
    int    dem_lru_capacity;           ///< DEM tiles held resident
    int    ocean_lru_capacity;         ///< ocean-mask tiles held resident

    // ── Data paths ───────────────────────────────────────────────────
    std::string dem_dir;               ///< directory containing USGS GeoTIFF tiles
    std::string gshhg_path;            ///< path to gshhs_f.b full-resolution GSHHG file

    /// Parse a pipeline config file. Lines are `key = value`; `#` begins a
    /// comment; blank lines are ignored. Throws std::runtime_error if the file
    /// cannot be opened or a required key is missing.
    static PipelineConfig load(const std::string& path);
};
