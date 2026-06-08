# Header-only tile-identity module.  Depended on by dem_tile_loader,
# ocean_mask_rasterizer, and compute_azimuth_range so all three agree on a
# single typed tile key (see CONTEXT.md "Tile Identity (GeoTile)").
add_library(geo INTERFACE)
target_include_directories(geo INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_geo_tile tests/test_geo_tile.cpp)
target_link_libraries(test_geo_tile PRIVATE geo)
add_test(NAME GeoTile COMMAND test_geo_tile)
