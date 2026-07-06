#include "PngWrite.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace {

void put_be32(uint8_t out[4], uint32_t x) {
    out[0] = static_cast<uint8_t>(x >> 24);
    out[1] = static_cast<uint8_t>(x >> 16);
    out[2] = static_cast<uint8_t>(x >> 8);
    out[3] = static_cast<uint8_t>(x);
}

// One PNG chunk: length, type, data, CRC32 over type+data.
bool write_chunk(FILE* f, const char type[4], const uint8_t* data, size_t n) {
    uint8_t be[4];
    put_be32(be, static_cast<uint32_t>(n));
    if (std::fwrite(be, 1, 4, f) != 4) return false;
    if (std::fwrite(type, 1, 4, f) != 4) return false;
    if (n > 0 && std::fwrite(data, 1, n, f) != n) return false;
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    if (n > 0) crc = crc32(crc, data, static_cast<uInt>(n));
    put_be32(be, static_cast<uint32_t>(crc));
    return std::fwrite(be, 1, 4, f) == 4;
}

}  // namespace

bool write_gray_png(const std::string& path, int width, int height,
                    const std::vector<uint8_t>& pixels) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<size_t>(width) * height)
        return false;

    // Raw scanline stream: each row prefixed with filter byte 0 (None).
    const size_t stride = static_cast<size_t>(width) + 1;
    std::vector<uint8_t> raw(stride * height);
    for (int r = 0; r < height; ++r) {
        raw[r * stride] = 0;
        std::memcpy(&raw[r * stride + 1], &pixels[static_cast<size_t>(r) * width],
                    width);
    }

    uLongf comp_len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> comp(comp_len);
    if (compress2(comp.data(), &comp_len, raw.data(),
                  static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK)
        return false;
    comp.resize(comp_len);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    bool ok = true;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    ok = ok && std::fwrite(sig, 1, 8, f) == 8;

    uint8_t ihdr[13];
    put_be32(ihdr, static_cast<uint32_t>(width));
    put_be32(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8]  = 8;  // bit depth
    ihdr[9]  = 0;  // color type: grayscale
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    ok = ok && write_chunk(f, "IHDR", ihdr, sizeof ihdr);
    ok = ok && write_chunk(f, "IDAT", comp.data(), comp.size());
    ok = ok && write_chunk(f, "IEND", nullptr, 0);

    ok = (std::fclose(f) == 0) && ok;
    return ok;
}
