find_package(GDAL REQUIRED)

add_executable(compute_azimuth_range main.cpp)
target_link_libraries(compute_azimuth_range PRIVATE
    horizon_sweep_engine
    dem_tile_loader
    ocean_mask_rasterizer
    GDAL::GDAL
)

add_executable(test_azimuth_range_accumulator
    tests/test_azimuth_range_accumulator.cpp
)
target_include_directories(test_azimuth_range_accumulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_azimuth_range_accumulator PRIVATE
    horizon_sweep_engine
)
add_test(NAME AzimuthRangeAccumulator COMMAND test_azimuth_range_accumulator)
