find_package(GDAL REQUIRED)
find_package(Boost REQUIRED)

add_library(ocean_mask_rasterizer OceanMaskRasterizer.cpp)
target_include_directories(ocean_mask_rasterizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocean_mask_rasterizer PRIVATE GDAL::GDAL)
target_include_directories(ocean_mask_rasterizer PRIVATE ${Boost_INCLUDE_DIRS})

add_executable(test_ocean_mask tests/test_ocean_mask.cpp)
target_link_libraries(test_ocean_mask PRIVATE ocean_mask_rasterizer GDAL::GDAL)
target_include_directories(test_ocean_mask PRIVATE ${Boost_INCLUDE_DIRS})

add_test(NAME OceanMask COMMAND test_ocean_mask)
