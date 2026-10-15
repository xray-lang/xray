add_executable(test_xir_unit_values test_unit_values.c)
target_link_libraries(test_xir_unit_values PRIVATE xray_xir_vm xray_xir_cgen)
set(unit_leaf_source "${CMAKE_CURRENT_BINARY_DIR}/unit_leaf_generated.c")
add_custom_command(OUTPUT "${unit_leaf_source}"
    COMMAND $<TARGET_FILE:test_xir_unit_values> --emit-leaf "${unit_leaf_source}"
    DEPENDS test_xir_unit_values
    VERBATIM)
add_executable(test_xir_unit_values_native test_unit_leaf_native.c "${unit_leaf_source}")
target_link_libraries(test_xir_unit_values_native PRIVATE xray_xir_vm)
foreach(target IN ITEMS test_xir_unit_values test_xir_unit_values_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30 LABELS "unit;xir;unit-value;ownership;parameter;leaf")
endforeach()
add_test(NAME test_xir_public_cli_unit_arguments
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_unit_arguments.py"
        --binary $<TARGET_FILE:xray> --root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_public_cli_unit_arguments PROPERTIES
    TIMEOUT 180 LABELS "unit;xir;unit-value;public-cli;native;ownership;ctfe")
add_test(NAME test_xir_semantic76_fields
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/../derive_semantic76_vectors.py")
set_tests_properties(test_xir_semantic76_fields PROPERTIES TIMEOUT 30 LABELS "unit;xir;wire;oracle;unit-value")
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
