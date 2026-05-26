add_library(ocean_mask_rasterizer OceanMaskRasterizer.cpp)
target_include_directories(ocean_mask_rasterizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_gshhg_open tests/test_gshhg_open.cpp)
target_link_libraries(test_gshhg_open PRIVATE ocean_mask_rasterizer)
target_compile_definitions(test_gshhg_open PRIVATE
    GSHHG_FULL_PATH="${CMAKE_SOURCE_DIR}/data/GSHHG/data/0-data/gshhs_f.b"
)

add_test(NAME GshhgOpen COMMAND test_gshhg_open)
