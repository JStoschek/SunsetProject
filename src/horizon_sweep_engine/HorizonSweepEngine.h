#pragma once
#include <vector>

#include "OceanMaskRasterizer.h"  // OceanOriginResult (struct only; no GDAL pulled in)
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

/// Coastline crossing finder. Production wraps OceanMaskRasterizer; tests
/// inject a FakeCoast with a configured crossing. `bearing` is the ocean->land
/// march bearing = (sunset_az + 180) mod 360.
struct CoastlineFinder {
    virtual ~CoastlineFinder() = default;
    virtual OceanOriginResult ocean_origin_for_ray(double bearing,
                                                    double lat, double lon) = 0;
};

/// Computes one Azimuth Slice per call by sweeping parallel rays inland from
/// the coast and maintaining a running maximum slope (the Horizon Sweep). The
/// line of sight is referenced to the Horizon Reference — the coastline
/// crossing offset seaward — with earth curvature and refraction subtracted
/// (ADR-0008). Depends on abstractions, not the file-backed loaders (ADR-0009).
class HorizonSweepEngine {
public:
    HorizonSweepEngine(ElevationSource&      dem,
                       CoastlineFinder&      ocean,
                       const PipelineConfig& config,
                       double min_lat, double max_lat,
                       double min_lon, double max_lon);

    /// Fill `out` with the visibility raster for the given SUNSET azimuth
    /// (255-285°). The buffer is sized once (from the bbox + cell size) and
    /// reused across calls: each call zeroes and refills the same buffer.
    void compute_slice(double azimuth_deg, AzimuthSlice& out);

    int width()  const { return width_; }
    int height() const { return height_; }

private:
    ElevationSource& dem_;
    CoastlineFinder& ocean_;
    PipelineConfig   config_;

    double min_lat_, max_lat_, min_lon_, max_lon_;
    double cell_deg_;   ///< degrees per cell = 1 / cell_per_degree
    int    width_, height_;

    std::vector<float> profile_;  ///< reused Phase-1 obstruction buffer
};
