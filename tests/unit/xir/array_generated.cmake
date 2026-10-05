# One real Source product replaces only the original fifteen Array generators.
set(XIR_ARRAY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_generated.c)
set(XIR_ARRAY_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_generated)
add_executable(test_xir_array_generated_source xir/test_xir_array_generated_source.c)
target_link_libraries(test_xir_array_generated_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_array_generated_source PRIVATE
    XR_ARRAY_FIXTURES="${XIR_ARRAY_FIXTURE}" XR_ARRAY_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_ARRAY_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_array_generated_source> ${XIR_ARRAY_GENERATED}
    DEPENDS test_xir_array_generated_source ${XIR_ARRAY_FIXTURE}/root.xr VERBATIM)
set(array_native_names array_alias array_strings array_empty array_integers array_nested
    array_allocations_0 array_allocations_1 array_allocations_2 array_allocations_3 array_allocations_4
    array_default array_default_allocations array_append array_append_allocations array_append_managed_allocations)
set(array_native_exits 0 2 0 2 1 0 0 0 0 0 0 0 0 0 0)
set(array_case_index 0)
foreach(array_case_name IN LISTS array_native_names)
    set(array_target test_xr_program_aot_${array_case_name})
    add_executable(${array_target} xir/test_xir_array_generated_native.c ${XIR_ARRAY_GENERATED})
    target_link_libraries(${array_target} PRIVATE xray_xir_scalar)
    target_compile_definitions(${array_target} PRIVATE XR_ARRAY_CASE=${array_case_index})
    xr_enable_pure_aot_symbol_map(${array_target})
    list(GET array_native_exits ${array_case_index} array_expected_exit)
    add_test(NAME ${array_target} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:${array_target}> --expected-exit ${array_expected_exit})
    set_tests_properties(${array_target} PROPERTIES LABELS "unit;xir;aot;generated-c;native;task-310;ownership" TIMEOUT 300)
    list(APPEND array_all_targets ${array_target})
    math(EXPR array_case_index "${array_case_index}+1")
endforeach()
list(APPEND array_all_targets test_xir_array_generated_source)
foreach(array_target IN LISTS array_all_targets)
    target_include_directories(${array_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
        (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${array_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${array_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_generated_source COMMAND $<TARGET_FILE:test_xir_array_generated_source>)
set_tests_properties(test_xir_array_generated_source PROPERTIES LABELS "unit;xir;source;vm;ownership" TIMEOUT 300)
add_test(NAME test_xir_array_generated_compiler COMMAND $<TARGET_FILE:test_xir_array_generated_source> --compiler)
set_tests_properties(test_xir_array_generated_compiler PROPERTIES
    LABELS "unit;xir;source;compiler;ownership;oom" TIMEOUT 300 RUN_SERIAL TRUE)
add_test(NAME test_xir_array_generated_admission COMMAND $<TARGET_FILE:test_xir_array_generated_source> --admission)
set_tests_properties(test_xir_array_generated_admission PROPERTIES LABELS "unit;xir;source;admission;ownership" TIMEOUT 300)
add_test(NAME test_xir_array_generated_native_compiler COMMAND $<TARGET_FILE:test_xr_program_aot_array_alias> --compiler)
set_tests_properties(test_xir_array_generated_native_compiler PROPERTIES
    LABELS "unit;xir;aot;compiler;ownership;oom" TIMEOUT 300 RUN_SERIAL TRUE)
