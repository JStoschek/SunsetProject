find_package(GDAL REQUIRED)

# ── Shared sweep library (sweep_strip + ThreadPool, no GDAL dependency) ───────
add_library(azimuth_range_sweep STATIC AzimuthRangeSweep.cpp)
target_include_directories(azimuth_range_sweep PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(azimuth_range_sweep PUBLIC horizon_sweep_engine bit_layout)

# ── Per-strip orchestration (freeze → sweep → water-mask) ─────────────────────
add_library(strip_processor STATIC StripProcessor.cpp)
target_include_directories(strip_processor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(strip_processor PUBLIC
    azimuth_range_sweep
    dem_tile_loader
    ocean_mask_rasterizer
)

# ── Packed-bitmask GeoTIFF writer (multi-band uint8 + azimuth metadata tags) ──
add_library(bitmask_geotiff_writer STATIC BitmaskGeoTiffWriter.cpp)
target_include_directories(bitmask_geotiff_writer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitmask_geotiff_writer PUBLIC
    azimuth_range_sweep  # StripResult
    bit_layout
    GDAL::GDAL
)

# ── Main binary ───────────────────────────────────────────────────────────────
add_executable(compute_azimuth_range main.cpp)
target_link_libraries(compute_azimuth_range PRIVATE
    strip_processor
    bitmask_geotiff_writer
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
    bit_layout
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
target_link_libraries(test_strip_working_set PRIVATE geo)
add_test(NAME StripWorkingSet COMMAND test_strip_working_set)

add_executable(test_strip_post_process tests/test_strip_post_process.cpp)
target_include_directories(test_strip_post_process PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_strip_post_process PRIVATE
    horizon_sweep_engine
    bit_layout
)
add_test(NAME StripPostProcess COMMAND test_strip_post_process)

add_executable(test_strip_processor tests/test_strip_processor.cpp)
target_link_libraries(test_strip_processor PRIVATE strip_processor GDAL::GDAL)
target_compile_definitions(test_strip_processor PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME StripProcessor COMMAND test_strip_processor)

add_executable(test_bitmask_geotiff_writer tests/test_bitmask_geotiff_writer.cpp)
target_link_libraries(test_bitmask_geotiff_writer PRIVATE
    bitmask_geotiff_writer
    GDAL::GDAL
)
add_test(NAME BitmaskGeoTiffWriter COMMAND test_bitmask_geotiff_writer)

add_executable(test_serial_vs_parallel tests/test_serial_vs_parallel.cpp)
target_link_libraries(test_serial_vs_parallel PRIVATE azimuth_range_sweep)
target_include_directories(test_serial_vs_parallel PRIVATE
    ${CMAKE_SOURCE_DIR}/src/horizon_sweep_engine
)
target_compile_definitions(test_serial_vs_parallel PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME SerialVsParallel COMMAND test_serial_vs_parallel)
