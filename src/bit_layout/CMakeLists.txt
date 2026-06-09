# The bit-layout contract: the single source of the azimuth-bitmask packing math
# (ADR-0013). Header-only, like geo. Mirrored in frontend/bit_layout.js and pinned
# by the shared vectors.json so the C++ packer and JS reader cannot drift.
add_library(bit_layout INTERFACE)
target_include_directories(bit_layout INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Transcode the shared committed vectors into a C++ header at build time, so the
# C++ test asserts against the exact same file the JS test reads (no JSON parser
# in the C++ build).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_vectors_json ${CMAKE_CURRENT_SOURCE_DIR}/vectors.json)
set(_vectors_header ${CMAKE_CURRENT_BINARY_DIR}/generated/bit_layout_vectors.h)
add_custom_command(
    OUTPUT ${_vectors_header}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gen_vectors_header.py
            ${_vectors_json} ${_vectors_header}
    DEPENDS ${_vectors_json} ${CMAKE_CURRENT_SOURCE_DIR}/gen_vectors_header.py
    COMMENT "Generating bit_layout_vectors.h from vectors.json")

# Listing the generated header as a source makes the test depend on the codegen.
add_executable(test_bit_layout tests/test_bit_layout.cpp ${_vectors_header})
target_link_libraries(test_bit_layout PRIVATE bit_layout)
target_include_directories(test_bit_layout PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_test(NAME BitLayout COMMAND test_bit_layout)
