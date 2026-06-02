find_package(GDAL REQUIRED)

# ── Shared sweep library (sweep_strip + ThreadPool, no GDAL dependency) ───────
add_library(azimuth_range_sweep STATIC AzimuthRangeSweep.cpp)
target_include_directories(azimuth_range_sweep PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(azimuth_range_sweep PUBLIC horizon_sweep_engine)

# ── Main binary ───────────────────────────────────────────────────────────────
add_executable(compute_azimuth_range main.cpp)
target_link_libraries(compute_azimuth_range PRIVATE
    azimuth_range_sweep
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

add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_include_directories(test_thread_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME ThreadPool COMMAND test_thread_pool)

set(PIPELINE_CONF "${CMAKE_SOURCE_DIR}/config/pipeline.conf")

add_executable(test_strip_working_set tests/test_strip_working_set.cpp)
target_include_directories(test_strip_working_set PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME StripWorkingSet COMMAND test_strip_working_set)

add_executable(test_strip_post_process tests/test_strip_post_process.cpp)
target_include_directories(test_strip_post_process PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_strip_post_process PRIVATE
    horizon_sweep_engine
)
add_test(NAME StripPostProcess COMMAND test_strip_post_process)

add_executable(test_serial_vs_parallel tests/test_serial_vs_parallel.cpp)
target_link_libraries(test_serial_vs_parallel PRIVATE azimuth_range_sweep)
target_include_directories(test_serial_vs_parallel PRIVATE
    ${CMAKE_SOURCE_DIR}/src/horizon_sweep_engine
)
target_compile_definitions(test_serial_vs_parallel PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME SerialVsParallel COMMAND test_serial_vs_parallel)
