cmake_minimum_required(VERSION 3.20)
project(SunsetProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)

enable_testing()
add_subdirectory(src/dem_tile_loader)
