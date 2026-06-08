find_package(GDAL REQUIRED)

add_library(dem_tile_loader DEMTileLoader.cpp)
target_include_directories(dem_tile_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dem_tile_loader PUBLIC geo PRIVATE GDAL::GDAL)

add_executable(test_tile_discovery tests/test_tile_discovery.cpp)
target_link_libraries(test_tile_discovery PRIVATE dem_tile_loader)
target_compile_definitions(test_tile_discovery PRIVATE
    DEM_DATA_DIR="${CMAKE_SOURCE_DIR}/data/DEM"
)

add_test(NAME TileDiscovery COMMAND test_tile_discovery)

add_executable(test_elevation_sentinels tests/test_elevation_sentinels.cpp)
target_link_libraries(test_elevation_sentinels PRIVATE dem_tile_loader GDAL::GDAL)

add_test(NAME ElevationSentinels COMMAND test_elevation_sentinels)

add_executable(test_bilinear_interpolation tests/test_bilinear_interpolation.cpp)
target_link_libraries(test_bilinear_interpolation PRIVATE dem_tile_loader GDAL::GDAL)

add_test(NAME BilinearInterpolation COMMAND test_bilinear_interpolation)

add_executable(test_lru_eviction tests/test_lru_eviction.cpp)
target_link_libraries(test_lru_eviction PRIVATE dem_tile_loader GDAL::GDAL)

add_test(NAME LruEviction COMMAND test_lru_eviction)

find_package(Threads REQUIRED)
add_executable(test_frozen_dem_view tests/test_frozen_dem_view.cpp)
target_link_libraries(test_frozen_dem_view PRIVATE dem_tile_loader GDAL::GDAL Threads::Threads)

add_test(NAME FrozenDemView COMMAND test_frozen_dem_view)
