#include "BitmaskGeoTiffWriter.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

namespace {

// Format a tunable as a compact, round-trippable decimal string ("233", "1",
// "1.5") for a GeoTIFF metadata tag.
std::string num_tag(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

}  // namespace

BitmaskGeoTiffWriter::BitmaskGeoTiffWriter(const std::string& path,
                                           int total_width, int total_height,
                                           double min_lon, double max_lat,
                                           double cell_deg,
                                           const BitLayout& layout,
                                           int format_version)
    : bytes_per_pixel_(layout.bytes_per_pixel) {
    GDALAllRegister();

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!drv) throw std::runtime_error("GTiff GDAL driver not available");

    char** create_opts = nullptr;
    create_opts = CSLSetNameValue(create_opts, "COMPRESS", "DEFLATE");
    create_opts = CSLSetNameValue(create_opts, "TILED",    "YES");

    ds_ = drv->Create(path.c_str(), total_width, total_height,
                      bytes_per_pixel_, GDT_Byte, create_opts);
    CSLDestroy(create_opts);
    if (!ds_)
        throw std::runtime_error("could not create GeoTIFF '" + path + "'");

    const double gt[6] = { min_lon, cell_deg, 0.0, max_lat, 0.0, -cell_deg };
    ds_->SetGeoTransform(const_cast<double*>(gt));

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    ds_->SetProjection(wkt);
    CPLFree(wkt);

    // Azimuth/format metadata — the self-describing wire contract (ADR-0013).
    // The encoder reads these instead of re-declaring the window.
    ds_->SetMetadataItem("azimuth_min_deg",  num_tag(layout.azimuth_min_deg).c_str());
    ds_->SetMetadataItem("azimuth_max_deg",  num_tag(layout.azimuth_max_deg).c_str());
    ds_->SetMetadataItem("azimuth_step_deg", num_tag(layout.azimuth_step_deg).c_str());
    ds_->SetMetadataItem("bit_count",        num_tag(layout.bit_count).c_str());
    ds_->SetMetadataItem("format_version",   num_tag(format_version).c_str());

    for (int b = 0; b < bytes_per_pixel_; ++b) {
        const std::string desc = "mask_byte_" + std::to_string(b);
        ds_->GetRasterBand(b + 1)->SetDescription(desc.c_str());
    }
}

BitmaskGeoTiffWriter::~BitmaskGeoTiffWriter() { close(); }

void BitmaskGeoTiffWriter::close() {
    if (ds_) {
        GDALClose(ds_);
        ds_ = nullptr;
    }
}

void BitmaskGeoTiffWriter::write_strip(const StripResult& strip,
                                       int y_off_from_north) {
    const int W   = strip.width;
    const int H   = strip.height;
    const int bpp = bytes_per_pixel_;

    // One band at a time: gather that byte from every pixel, flipping the strip
    // vertically (strip row 0 = south; GeoTIFF row 0 = north).
    std::vector<std::uint8_t> band_buf(static_cast<std::size_t>(W) * H);
    for (int b = 0; b < bpp; ++b) {
        for (int r = 0; r < H; ++r) {
            const int tiff_row = H - 1 - r;
            for (int c = 0; c < W; ++c) {
                const std::size_t src = (static_cast<std::size_t>(r) * W + c) * bpp + b;
                band_buf[static_cast<std::size_t>(tiff_row) * W + c] = strip.mask[src];
            }
        }
        const CPLErr err = ds_->GetRasterBand(b + 1)->RasterIO(
            GF_Write, 0, y_off_from_north, W, H,
            band_buf.data(), W, H, GDT_Byte, 0, 0);
        if (err != CE_None)
            throw std::runtime_error("RasterIO write failed for mask band " +
                                     std::to_string(b + 1));
    }
}
