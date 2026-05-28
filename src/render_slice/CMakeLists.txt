find_package(GDAL REQUIRED)

add_executable(render_slice render_slice_main.cpp)
target_link_libraries(render_slice PRIVATE
    horizon_sweep_engine
    dem_tile_loader
    ocean_mask_rasterizer
    GDAL::GDAL
)
