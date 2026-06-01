find_package(GDAL REQUIRED)

add_library(ocean_mask_rasterizer OceanMaskRasterizer.cpp)
target_include_directories(ocean_mask_rasterizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocean_mask_rasterizer PRIVATE GDAL::GDAL)

add_executable(test_gshhg_open tests/test_gshhg_open.cpp)
target_link_libraries(test_gshhg_open PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_gshhg_open PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME GshhgOpen COMMAND test_gshhg_open)

add_executable(test_is_water_rasterization tests/test_is_water_rasterization.cpp)
target_link_libraries(test_is_water_rasterization PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_is_water_rasterization PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME IsWaterRasterization COMMAND test_is_water_rasterization)

add_executable(test_lru_tile_cache tests/test_lru_tile_cache.cpp)
target_link_libraries(test_lru_tile_cache PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_lru_tile_cache PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME LruTileCache COMMAND test_lru_tile_cache)

add_executable(test_ocean_origin_for_ray tests/test_ocean_origin_for_ray.cpp)
target_link_libraries(test_ocean_origin_for_ray PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_ocean_origin_for_ray PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME OceanOriginForRay COMMAND test_ocean_origin_for_ray)
# OceanOriginForRay includes Cycles 8-9 which pin the NW-sunset crossing for
# Ocean Beach — the regression guard for b350d3e-style coast-march changes.

add_executable(test_gshhg_spatial_index tests/test_gshhg_spatial_index.cpp)
target_link_libraries(test_gshhg_spatial_index PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_gshhg_spatial_index PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME GshhgSpatialIndex COMMAND test_gshhg_spatial_index)
