# Keep the original scenario-zero target and every other module-state target.
# The source-owned product supplies both independently checked execution modes.
set(XIR_STATE_ZERO_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_zero.c)
set(XIR_STATE_ZERO_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_module_state_zero)
add_executable(test_xir_module_state_zero_source xir/test_xir_module_state_zero_source.c)
target_link_libraries(test_xir_module_state_zero_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_module_state_zero_source PRIVATE
    XR_STATE_ZERO_FIXTURES="${XIR_STATE_ZERO_FIXTURE}"
    XR_STATE_ZERO_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_STATE_ZERO_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_module_state_zero_source> ${XIR_STATE_ZERO_GENERATED}
    DEPENDS test_xir_module_state_zero_source
        ${XIR_STATE_ZERO_FIXTURE}/root.xr ${XIR_STATE_ZERO_FIXTURE}/base.xr
        ${XIR_STATE_ZERO_FIXTURE}/left.xr ${XIR_STATE_ZERO_FIXTURE}/right.xr
    VERBATIM)
add_executable(test_xr_program_aot_module_state_0
    xir/test_xir_module_state_zero_native.c ${XIR_STATE_ZERO_GENERATED})
target_link_libraries(test_xr_program_aot_module_state_0 PRIVATE xray_xir_scalar)
foreach(state_zero_target IN ITEMS test_xir_module_state_zero_source test_xr_program_aot_module_state_0)
    target_include_directories(${state_zero_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
        (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${state_zero_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${state_zero_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${state_zero_target} COMMAND $<TARGET_FILE:${state_zero_target}>)
    set_tests_properties(${state_zero_target} PROPERTIES
        LABELS "unit;xir;canonical-program;execution;module;ownership;abi" TIMEOUT 300)
endforeach()
set_property(TEST test_xr_program_aot_module_state_0 APPEND PROPERTY LABELS "aot;generated-c;native;task-310")
