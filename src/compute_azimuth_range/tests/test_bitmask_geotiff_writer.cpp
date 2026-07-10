// Read-back test for BitmaskGeoTiffWriter — the I/O seam that turns a strip's
// packed bitmask (StripResult) into a multi-band uint8 GeoTIFF with the azimuth
// metadata tags (ADR-0013).  We write a synthetic strip, reopen the file with
// GDAL, and assert:
//   - band count == bytes_per_pixel
//   - the packed bits land at the right pixel, after the south→north flip
//   - all five azimuth/format metadata tags round-trip
//
// No DEM/ocean/sweep needed — the writer is driven with a hand-built StripResult.

#include "BitmaskGeoTiffWriter.h"

#include "AzimuthRangeSweep.h"  // StripResult
#include "BitLayout.h"

#include <gdal_priv.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Read all bands of one pixel out of an opened dataset into a bpp-byte span.
std::vector<std::uint8_t> read_pixel(GDALDataset* ds, int x, int y, int bpp) {
    std::vector<std::uint8_t> pixel(bpp, 0);
    for (int b = 0; b < bpp; ++b) {
        const CPLErr err = ds->GetRasterBand(b + 1)->RasterIO(
            GF_Read, x, y, 1, 1, &pixel[b], 1, 1, GDT_Byte, 0, 0);
        assert(err == CE_None);
    }
    return pixel;
}

double meta_num(GDALDataset* ds, const char* key) {
    const char* v = ds->GetMetadataItem(key);
    assert(v && "metadata tag missing");
    return std::atof(v);
}

}  // namespace

int main() {
    GDALAllRegister();

    // Production window so bytes_per_pixel == 10, like the real pipeline.
    const BitLayout layout = BitLayout::from_config(233.0, 309.0, 1.0);
    const int bpp = layout.bytes_per_pixel;
    assert(bpp == 10);

    const int W = 2, H = 3;
    const double min_lon = -123.0, max_lat = 38.0, cell_deg = 1.0 / 60.0;
    const int format_version = 1;

    // Synthetic single strip (row 0 = south).  Mark two pixels distinctly so we
    // can prove the vertical flip and the bit packing:
    //   south-west pixel (r=0,c=0): land + visible at 270° (bit 37)
    //   north-west pixel (r=H-1,c=0): land, no visibility (never-visible)
    StripResult strip;
    strip.width = W; strip.height = H; strip.bytes_per_pixel = bpp;
    strip.mask.assign(static_cast<std::size_t>(W) * H * bpp, 0);

    const int bit_270 = *layout.azimuth_to_bit_index(270.0);
    std::uint8_t* sw = strip.mask.data() + (0 * W + 0) * bpp;
    BitLayout::set_bit(sw, bit_270);
    layout.set_flag(sw);
    std::uint8_t* nw = strip.mask.data() + ((H - 1) * W + 0) * bpp;
    layout.set_flag(nw);

    const fs::path path = fs::temp_directory_path() /
        ("bitmask_writer_" + std::to_string(::getpid()) + ".tif");

    {
        BitmaskGeoTiffWriter writer(path.string(), W, H,
                                    min_lon, max_lat, cell_deg,
                                    layout, format_version);
        writer.write_strip(strip, /*y_off_from_north=*/0);
    }  // writer closed here

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpen(path.string().c_str(), GA_ReadOnly));
    assert(ds && "could not reopen written GeoTIFF");

    // ── Band count == bytes_per_pixel ─────────────────────────────────────
    assert(ds->GetRasterCount() == bpp);
    std::printf("PASS: band count == bytes_per_pixel (%d)\n", bpp);

    // ── Vertical flip + packed bits ───────────────────────────────────────
    // South pixel (strip row 0) must land at GeoTIFF bottom row (y = H-1).
    {
        const std::vector<std::uint8_t> p = read_pixel(ds, 0, H - 1, bpp);
        assert(layout.get_flag(p.data())            && "south pixel is land");
        assert(BitLayout::get_bit(p.data(), bit_270) && "south pixel visible at 270");
        assert(!BitLayout::get_bit(p.data(), bit_270 - 1) && "neighbour bit clear");
        std::puts("PASS: south pixel's 270° bit survives the flip at GeoTIFF bottom");
    }
    // North pixel (strip row H-1) must land at GeoTIFF top row (y = 0).
    {
        const std::vector<std::uint8_t> p = read_pixel(ds, 0, 0, bpp);
        assert(layout.get_flag(p.data()) && "north pixel is land");
        bool any = false;
        for (int b = 0; b < layout.bit_count; ++b)
            any = any || BitLayout::get_bit(p.data(), b);
        assert(!any && "north pixel never-visible: no visibility bits");
        std::puts("PASS: north never-visible pixel at GeoTIFF top, flag set");
    }

    // ── Metadata tags ─────────────────────────────────────────────────────
    assert(meta_num(ds, "azimuth_min_deg")  == 233.0);
    assert(meta_num(ds, "azimuth_max_deg")  == 309.0);
    assert(meta_num(ds, "azimuth_step_deg") == 1.0);
    assert((int)meta_num(ds, "bit_count")      == layout.bit_count);
    assert((int)meta_num(ds, "format_version") == format_version);
    std::puts("PASS: azimuth/format metadata tags round-trip");

    // ── Point registration: pixel centres sit on the engine's sample lattice ─
    // The engine samples col c at lon = min_lon + c*cell_deg and the top TIFF
    // row at lat = max_lat - cell_deg (AzimuthSlice geometry).  The origin must
    // therefore be offset half a cell so pixel *centres*, not corners, land on
    // those points — otherwise the overlay draws half a cell north-east of the
    // terrain it describes.
    {
        double gt[6];
        assert(ds->GetGeoTransform(gt) == CE_None);
        const double eps = 1e-12;
        assert(std::fabs(gt[0] - (min_lon - 0.5 * cell_deg)) < eps);
        assert(std::fabs(gt[3] - (max_lat - 0.5 * cell_deg)) < eps);
        assert(std::fabs(gt[1] - cell_deg) < eps);
        assert(std::fabs(gt[5] + cell_deg) < eps);
        assert(gt[2] == 0.0 && gt[4] == 0.0);

        // Centre of top-left pixel == first sample point (min_lon, max_lat-cell).
        const double cx = gt[0] + 0.5 * gt[1];
        const double cy = gt[3] + 0.5 * gt[5];
        assert(std::fabs(cx - min_lon) < eps);
        assert(std::fabs(cy - (max_lat - cell_deg)) < eps);

        const char* ap = ds->GetMetadataItem("AREA_OR_POINT");
        assert(ap && std::string(ap) == "Point");
        std::puts("PASS: geotransform is point-registered on the sample lattice");
    }

    GDALClose(ds);
    fs::remove(path);
    std::puts("ALL PASS");
    return 0;
}
