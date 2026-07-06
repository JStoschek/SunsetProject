#pragma once
#include <cstdint>
#include <string>
#include <vector>

/// Write an 8-bit grayscale PNG (zlib-compressed, one IDAT). `pixels` is
/// row-major, row 0 = top of the image, width*height bytes. Returns false on
/// I/O or compression failure. Kept dependency-light (zlib only) so debug
/// artifacts can be written from anywhere in the pipeline without GDAL.
bool write_gray_png(const std::string& path, int width, int height,
                    const std::vector<uint8_t>& pixels);
