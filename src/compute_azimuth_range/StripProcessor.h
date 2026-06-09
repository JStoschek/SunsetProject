#pragma once
#include "AzimuthRangeSweep.h"  // StripResult, ThreadPool, PipelineConfig

class DEMTileLoader;        // dem_tile_loader (GDAL-backed; kept out of this header)
class OceanMaskRasterizer;  // ocean_mask_rasterizer (GDAL-backed)

// Computes the packed Visible Azimuth Set bitmask for one Latitude Strip.  Owns
// the whole per-strip sequence behind a single interface:
//
//   strip_working_set → DEM/ocean freeze → FrozenStripSources → sweep_strip
//   → apply_land_data_flag
//
// The freeze-before-parallel structure (ADR-0011) lives here: each call freezes
// the strip's working set into eviction-free caches, runs the lock-free parallel
// sweep over them, sets the land/data flag, and lets the frozen caches die with
// the call.  The shared loaders and thread pool are reused across every strip.
//
// What stays outside the seam: all GeoTIFF I/O (band layout, the south→north
// vertical flip, RasterIO writes — see BitmaskGeoTiffWriter) remains in the
// caller, so this module is pure terrain orchestration and testable without a
// file.
class StripProcessor {
public:
    StripProcessor(DEMTileLoader&        dem,
                   OceanMaskRasterizer&  omr,
                   ThreadPool&           pool,
                   const PipelineConfig& config,
                   double min_lon, double max_lon);

    // Freeze → sweep → flag for the latitude band [strip_min_lat, strip_max_lat]
    // across the configured longitude extent.  Returns the packed per-pixel
    // azimuth bitmask with the land/data flag applied (row 0 = south).
    StripResult process(double strip_min_lat, double strip_max_lat);

private:
    DEMTileLoader&        dem_;
    OceanMaskRasterizer&  omr_;
    ThreadPool&           pool_;
    const PipelineConfig& config_;
    double                min_lon_;
    double                max_lon_;
};
