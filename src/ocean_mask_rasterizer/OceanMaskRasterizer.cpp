#include "OceanMaskRasterizer.h"
#include <bit>
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// GSHHG binary files are big-endian.  Byte-swap a single int32_t.
// ---------------------------------------------------------------------------
static int32_t be32(int32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    std::memcpy(&v, &u, 4);
#endif
    return v;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
OceanMaskRasterizer::OceanMaskRasterizer(const std::string& gshhg_full_path,
                                          int lru_capacity)
    : lru_capacity_(lru_capacity), path_(gshhg_full_path)
{
    file_.open(gshhg_full_path, std::ios::binary);
    if (!file_.is_open())
        throw std::runtime_error(
            "OceanMaskRasterizer: cannot open GSHHG file: " + gshhg_full_path);

    // Validate by reading the first polygon header.
    GshhgHeader hdr{};
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!file_)
        throw std::runtime_error(
            "OceanMaskRasterizer: file too short to contain a GSHHG header: "
            + gshhg_full_path);

    // Byte-swap from big-endian.
    hdr.id   = be32(hdr.id);
    hdr.n    = be32(hdr.n);
    hdr.flag = be32(hdr.flag);

    // First polygon must be id=0, have at least 1 point, and be level 1 (land).
    int level = hdr.flag & 0xFF;
    if (hdr.id != 0 || hdr.n < 1 || level != 1)
        throw std::runtime_error(
            "OceanMaskRasterizer: file does not look like a GSHHG binary: "
            + gshhg_full_path);

    // Rewind so downstream code can scan from the start.
    file_.seekg(0, std::ios::beg);
}

// ---------------------------------------------------------------------------
// Stubs — full implementations in later issues.
// ---------------------------------------------------------------------------

bool OceanMaskRasterizer::is_water(double /*lat*/, double /*lon*/)
{
    return false;
}

std::pair<double, double>
OceanMaskRasterizer::ocean_origin_for_ray(double /*azimuth_deg*/,
                                           double lat, double lon)
{
    return {lat, lon};
}
