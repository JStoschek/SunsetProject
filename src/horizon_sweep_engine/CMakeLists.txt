find_package(GDAL REQUIRED)

add_library(horizon_sweep_engine
    HorizonSweepEngine.cpp
    PipelineConfig.cpp
)
target_include_directories(horizon_sweep_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # OceanOriginResult struct (header-only use; no GDAL dependency pulled in).
    ${CMAKE_SOURCE_DIR}/src/ocean_mask_rasterizer
    ${CMAKE_SOURCE_DIR}/src/dem_tile_loader
)

# Path to the real pipeline config, injected so tests stay config-driven
# (no hard-coded tunables) — mirrors how sibling modules inject data paths.
set(PIPELINE_CONF "${CMAKE_SOURCE_DIR}/config/pipeline.conf")

add_executable(test_cardinal_visibility tests/test_cardinal_visibility.cpp)
target_link_libraries(test_cardinal_visibility PRIVATE horizon_sweep_engine)
target_compile_definitions(test_cardinal_visibility PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME CardinalVisibility COMMAND test_cardinal_visibility)

add_executable(test_rotated_frame_visibility tests/test_rotated_frame_visibility.cpp)
target_link_libraries(test_rotated_frame_visibility PRIVATE horizon_sweep_engine)
target_compile_definitions(test_rotated_frame_visibility PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME RotatedFrameVisibility COMMAND test_rotated_frame_visibility)

add_executable(test_nan_no_data tests/test_nan_no_data.cpp)
target_link_libraries(test_nan_no_data PRIVATE horizon_sweep_engine)
target_compile_definitions(test_nan_no_data PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME NaNNoData COMMAND test_nan_no_data)

add_executable(test_production_adapters tests/test_production_adapters.cpp)
target_link_libraries(test_production_adapters PRIVATE
    horizon_sweep_engine
    dem_tile_loader
    ocean_mask_rasterizer
    GDAL::GDAL
)
target_compile_definitions(test_production_adapters PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)
add_test(NAME ProductionAdaptersIntegration COMMAND test_production_adapters)
