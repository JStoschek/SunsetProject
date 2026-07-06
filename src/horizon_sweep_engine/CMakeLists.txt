find_package(GDAL REQUIRED)
find_package(ZLIB REQUIRED)  # PngWrite (debug grid dump / slice PNG artifacts)

add_library(horizon_sweep_engine
    HorizonSweepEngine.cpp
    PipelineConfig.cpp
    PngWrite.cpp
)
target_include_directories(horizon_sweep_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Loader headers used by ProductionAdapters.h (compiled only by targets
    # that link the file-backed loaders; no GDAL dependency pulled in here).
    ${CMAKE_SOURCE_DIR}/src/ocean_mask_rasterizer
    ${CMAKE_SOURCE_DIR}/src/dem_tile_loader
)
# Those headers key tiles on GeoTile; propagate the geo include path to all
# dependents (header-only, no link-time cost).
target_link_libraries(horizon_sweep_engine PUBLIC geo PRIVATE ZLIB::ZLIB)

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

add_executable(test_trace_ray_selection tests/test_trace_ray_selection.cpp)
target_link_libraries(test_trace_ray_selection PRIVATE horizon_sweep_engine)
target_compile_definitions(test_trace_ray_selection PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME TraceRaySelection COMMAND test_trace_ray_selection)
# TraceRaySelection pins march/gather/trace consistency (ADR-0014 amendment):
# the march steps straight in the rotated frame, so the ray that owns a pixel
# (round(perp/s)) is the ray that marches through it, and the trace dumps that
# exact ray. Reintroducing local-cos(lat) stepping drifts the march ~100 m off
# its frame line on this tall box and fails. FakeWater/FakeDEM, no file I/O.

add_executable(test_sample_verdicts tests/test_sample_verdicts.cpp)
target_link_libraries(test_sample_verdicts PRIVATE horizon_sweep_engine)
target_compile_definitions(test_sample_verdicts PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME SampleVerdicts COMMAND test_sample_verdicts)
# SampleVerdicts pins the per-sample verdict math of the forward-sampled grid
# (ADR-0014) with analytic synthetic DEMs: closed-form curvature horizon, ridge
# lee blocking, disjoint visible-again bands, sub-eye-height berms, and the
# d²c curvature term — asserted per sample via the retained Visibility Sample
# Grid. FakeWater/FakeDEM, no file I/O.

add_executable(test_ocean_beach tests/test_ocean_beach.cpp)
target_link_libraries(test_ocean_beach PRIVATE horizon_sweep_engine)
target_compile_definitions(test_ocean_beach PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME OceanBeach COMMAND test_ocean_beach)
# OceanBeach pins the known-visible Ocean Beach / Great Highway band at its NW
# summer-sunset azimuth — the exact regression a past perf change silently
# introduced (b350d3e blacked the beach out). FakeWater/FakeDEM, no file I/O.

add_executable(test_coastal_regression tests/test_coastal_regression.cpp)
target_link_libraries(test_coastal_regression PRIVATE horizon_sweep_engine)
target_compile_definitions(test_coastal_regression PRIVATE
    PIPELINE_CONF_PATH="${PIPELINE_CONF}"
)
add_test(NAME CoastalRegression COMMAND test_coastal_regression)
# CoastalRegression pins Ocean Beach visibility at az=300° (NW summer sunset)
# using FakeWater + FakeDEM (no file I/O).  It catches engine-level regressions
# that would shift along_c past Ocean Beach and silence the beach at NW azimuths.
