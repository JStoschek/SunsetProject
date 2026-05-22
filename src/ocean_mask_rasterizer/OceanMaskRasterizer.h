#pragma once
#include <cstddef>
#include <memory>
#include <string>

// Loads NHD HR SeaOcean (FType 445) polygons from a GDAL-readable dataset
// (typically an OpenFileGDB .gdb) and answers point-in-polygon queries.
// Construction is the only I/O; all subsequent queries are in-memory.
// Boundary points are considered ocean (OGRGeometry::Intersects semantics).
class OceanMaskRasterizer {
public:
    explicit OceanMaskRasterizer(const std::string& path);
    ~OceanMaskRasterizer();

    // Returns true iff (lat, lon) lies inside or on the boundary of a SeaOcean polygon.
    bool is_ocean(double lat, double lon) const;

    std::size_t polygon_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
