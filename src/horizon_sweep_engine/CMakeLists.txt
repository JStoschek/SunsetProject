add_library(horizon_sweep_engine
    HorizonSweepEngine.cpp
    PipelineConfig.cpp
)
target_include_directories(horizon_sweep_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # OceanOriginResult struct (header-only use; no GDAL dependency pulled in).
    ${CMAKE_SOURCE_DIR}/src/ocean_mask_rasterizer
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
