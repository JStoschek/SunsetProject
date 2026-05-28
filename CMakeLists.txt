cmake_minimum_required(VERSION 3.20)
project(SunsetProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)

enable_testing()
add_subdirectory(src/dem_tile_loader)
add_subdirectory(src/ocean_mask_rasterizer)
add_subdirectory(src/horizon_sweep_engine)
add_subdirectory(src/render_slice)
