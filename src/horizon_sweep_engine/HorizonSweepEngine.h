#pragma once
#include <functional>
#include <string>
#include <vector>

#include "PipelineConfig.h"

/// A binary visibility raster for a single sunset azimuth. Row-major: pixel
/// (row, col) is at index `row * width + col`. Geographic layout:
///   lon(col) = min_lon + col / cell_per_degree   (col 0 = western edge)
///   lat(row) = min_lat + row / cell_per_degree   (row 0 = southern edge)
struct AzimuthSlice {
    std::vector<bool> visible;  ///< true iff the land pixel can see the sunset
    int    width  = 0;
    int    height = 0;
    double min_lat = 0.0;
    double min_lon = 0.0;
};

/// Terrain elevation source. Production wraps DEMTileLoader; tests inject a
/// FakeDEM built from an analytic h(lat, lon) lambda. NaN means "no data".
struct ElevationSource {
    virtual ~ElevationSource() = default;
    virtual float get_elevation(double lat, double lon) = 0;
};

/// Water lookup. Production wraps the Ocean Mask (OceanMaskRasterizer or a
/// FrozenOceanView); tests inject a FakeWater. Coast-finding is folded into
/// the engine's per-ray march (ADR-0014), so this point query is the only
/// ocean seam the engine needs.
struct WaterQuery {
    virtual ~WaterQuery() = default;
    virtual bool is_water(double lat, double lon) = 0;
};

/// Computes one Azimuth Slice per call from a forward-sampled Visibility
/// Sample Grid (ADR-0014). For each sunset azimuth the engine casts parallel
/// rays in the rotated frame (ADR-0007), each a single march from the box's
/// offshore western edge: step at the sample spacing testing is_water until
/// the coastline crossing, then continue inland — comparing Horizon Reaches
/// (ADR-0016): a sample is visible iff its own sea horizon, sqrt(h/c) minus
/// its inland distance, extends at least as far seaward as every western
/// obstruction's — storing a finished visible/not verdict at every sample.
/// The output slice is derived by inverse nearest-neighbour gather: each
/// pixel inherits its nearest sample's verdict; no per-pixel physics
/// re-evaluation. Depends on abstractions, not the file-backed loaders
/// (ADR-0009).
class HorizonSweepEngine {
public:
    HorizonSweepEngine(ElevationSource&      dem,
                       WaterQuery&           water,
                       const PipelineConfig& config,
                       double min_lat, double max_lat,
                       double min_lon, double max_lon);

    /// Fill `out` with the visibility raster for the given SUNSET azimuth.
    /// The buffer is sized once (from the bbox + cell size) and reused across
    /// calls: each call zeroes and refills the same buffer.
    void compute_slice(double azimuth_deg, AzimuthSlice& out);

    /// Diagnostic per-step ray trace (off by default; zero overhead when off).
    /// When enabled, compute_slice traces the ray that OWNS the target pixel —
    /// round(perp/s), the gather's own assignment, which (because rays march
    /// straight in the rotated frame) is also the ray whose march passes
    /// within half a sample spacing of the target. It prints every march
    /// sample from `steps_before` steps seaward of the coastline crossing
    /// through `steps_after` steps inland. Each printed row shows the sample's
    /// STORED verdict — the very bool the gather hands to output pixels — so
    /// the trace shows exactly where the map came from, not a re-derivation.
    /// `water_query` (optional) supplies the is_water column for context.
    struct RayTrace {
        bool   enabled      = false;
        double target_lat   = 0.0;
        double target_lon   = 0.0;
        int    steps_before = 100;  ///< steps seaward of the crossing to print
        int    steps_after  = 100;  ///< steps inland of the crossing to print
        std::function<bool(double lat, double lon)> water_query;  ///< optional
        /// Optional sink: when tracing, compute_slice reports the resolved ray
        /// index and that ray's coast crossing. Lets tests observe ray
        /// *selection* without scraping stdout. Off by default; never fires on
        /// the production sweep (trace disabled).
        std::function<void(long ray_index, double coast_lat, double coast_lon)>
            on_resolved;
    };
    void set_trace(const RayTrace& trace) { trace_ = trace; }

    /// One ray of the Visibility Sample Grid, as retained by the full-grid
    /// debug dump. `verdicts` is indexed from the coast crossing (sample 0 =
    /// first land); empty when the ray found no coast within the give-up
    /// distance.
    struct GridRay {
        double along_coast = 0.0;   ///< along-frame coordinate of the crossing
        double coast_lat   = 0.0;
        double coast_lon   = 0.0;
        std::vector<char> verdicts; ///< 1 = visible, 0 = blocked
    };
    /// The whole grid for one azimuth: rays[j - j_min] is ray index j.
    struct SampleGrid {
        long j_min = 0;
        long j_max = -1;            ///< empty grid when j_max < j_min
        std::vector<GridRay> rays;
    };

    /// Full-grid debug dump (off by default; zero cost when off). When
    /// `retain` is set, the next compute_slice keeps every ray's
    /// (along_coast, verdicts) — readable via sample_grid() — and, if
    /// `png_path` is non-empty, writes the grid to disk in the ray frame
    /// (rows = ray index j, cols = sample index k) as an 8-bit PNG:
    /// 255 = visible sample, 96 = blocked sample, 0 = past the ray's end or
    /// no coast. Intended for single-azimuth debugging only: retention holds
    /// the whole grid in memory.
    void set_grid_dump(bool retain, std::string png_path = std::string()) {
        retain_grid_    = retain;
        grid_png_path_  = std::move(png_path);
    }
    const SampleGrid& sample_grid() const { return grid_; }

    int width()  const { return width_; }
    int height() const { return height_; }

private:
    ElevationSource& dem_;
    WaterQuery&      water_;
    PipelineConfig   config_;

    double min_lat_, max_lat_, min_lon_, max_lon_;
    double cell_deg_;   ///< degrees per cell = 1 / cell_per_degree
    int    width_, height_;

    std::vector<char> verdicts_;  ///< reused per-ray verdict buffer
    RayTrace          trace_;     ///< diagnostic trace config (disabled by default)

    bool        retain_grid_ = false;  ///< full-grid debug dump switch
    std::string grid_png_path_;        ///< optional PNG destination for the dump
    SampleGrid  grid_;                 ///< retained grid (empty unless enabled)
};

/// Write a retained Visibility Sample Grid as an 8-bit grayscale PNG in the
/// ray frame: rows = ray index j (top row = j_min), cols = sample index k
/// (col 0 = each ray's coast crossing). 255 = visible, 96 = blocked, 0 = past
/// the ray's end / no coast. Returns false on I/O failure or an empty grid.
bool write_sample_grid_png(const HorizonSweepEngine::SampleGrid& grid,
                           const std::string& path);
