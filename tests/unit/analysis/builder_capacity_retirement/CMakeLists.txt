add_executable(test_builder_capacity_retirement "${CMAKE_CURRENT_LIST_DIR}/test_builder_capacity_retirement.c"
    "${PROJECT_SOURCE_DIR}/src/analysis/xglobal_producer.c"
    "${PROJECT_SOURCE_DIR}/src/analysis/xglobal_summary.c"
    "${PROJECT_SOURCE_DIR}/src/aot/xaot_bundle.c"
    "${PROJECT_SOURCE_DIR}/src/aot/xaot_verify.c"
    "${PROJECT_SOURCE_DIR}/src/base/xmemstream.c"
    "${PROJECT_SOURCE_DIR}/src/base/xhash.c"
    "${PROJECT_SOURCE_DIR}/src/base/xtarget_data_layout.c")
target_link_libraries(test_builder_capacity_retirement PRIVATE
    xray_compiler_parser xray_xir_source)
target_include_directories(test_builder_capacity_retirement PRIVATE ${XRAY_COMMON_INCLUDES}
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}/generated")
set_target_properties(test_builder_capacity_retirement PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_builder_capacity_retirement PRIVATE /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_builder_capacity_retirement PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_builder_capacity_retirement PRIVATE -Wall -Wextra)
endif()
add_test(NAME builder_capacity_retirement COMMAND test_builder_capacity_retirement)
set_tests_properties(builder_capacity_retirement PROPERTIES LABELS "unit;analysis;aot;ownership" TIMEOUT 30)
