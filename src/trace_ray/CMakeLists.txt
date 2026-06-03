find_package(GDAL REQUIRED)

# Diagnostic single-ray trace tool: drives the real engine + production loaders.
add_executable(trace_ray main.cpp)
target_link_libraries(trace_ray PRIVATE
    horizon_sweep_engine
    dem_tile_loader
    ocean_mask_rasterizer
    GDAL::GDAL
)
