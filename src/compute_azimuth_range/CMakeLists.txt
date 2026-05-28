add_executable(compute_azimuth_range main.cpp)
target_link_libraries(compute_azimuth_range PRIVATE
    horizon_sweep_engine
)
