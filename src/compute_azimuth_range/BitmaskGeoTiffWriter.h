#pragma once
#include <string>

#include "AzimuthRangeSweep.h"  // StripResult
#include "BitLayout.h"

class GDALDataset;

/// Writes a Latitude Strip's packed per-pixel azimuth bitmask to a multi-band
/// uint8 GeoTIFF (ADR-0013).  Owns the output dataset for its lifetime: the
/// constructor creates an EPSG:4326 north-up raster of bytes_per_pixel uint8
/// bands and stamps the azimuth/format metadata tags; write_strip() de-interleaves
/// each strip's pixel bytes into the bands, flipping south→north; the destructor
/// (or close()) flushes and closes the file.
///
/// This is the I/O seam kept out of StripProcessor: the south→north flip and the
/// GDAL band writes live here, driven by the pure StripResult the sweep produces.
class BitmaskGeoTiffWriter {
public:
    /// Create the output GeoTIFF.  `min_lon`/`max_lat` are the north-west corner
    /// and `cell_deg` the pixel size of the north-up grid.  Throws
    /// std::runtime_error if the driver or file cannot be created.
    BitmaskGeoTiffWriter(const std::string& path,
                         int total_width, int total_height,
                         double min_lon, double max_lat, double cell_deg,
                         const BitLayout& layout, int format_version);

    ~BitmaskGeoTiffWriter();

    BitmaskGeoTiffWriter(const BitmaskGeoTiffWriter&)            = delete;
    BitmaskGeoTiffWriter& operator=(const BitmaskGeoTiffWriter&) = delete;

    /// De-interleave `strip`'s pixel-interleaved mask into the bytes_per_pixel
    /// bands and write it at `y_off_from_north` rows below the top of the output,
    /// flipping each strip vertically (strip row 0 = south → GeoTIFF bottom row).
    /// Throws std::runtime_error on a RasterIO failure.
    void write_strip(const StripResult& strip, int y_off_from_north);

    /// Flush and close the dataset; idempotent.  Called by the destructor.
    void close();

private:
    GDALDataset* ds_ = nullptr;
    int          bytes_per_pixel_;
};
