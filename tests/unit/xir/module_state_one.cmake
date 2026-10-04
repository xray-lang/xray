# Keep the original scenario-one target and every other module-state target.
# The source-owned product supplies both independently checked execution modes.
set(XIR_STATE_ONE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_one.c)
set(XIR_STATE_ONE_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_module_state_one)
add_executable(test_xir_module_state_one_source xir/test_xir_module_state_one_source.c)
target_link_libraries(test_xir_module_state_one_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_module_state_one_source PRIVATE
    XR_STATE_ONE_FIXTURES="${XIR_STATE_ONE_FIXTURE}"
    XR_STATE_ONE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_STATE_ONE_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_module_state_one_source> ${XIR_STATE_ONE_GENERATED}
    DEPENDS test_xir_module_state_one_source
        ${XIR_STATE_ONE_FIXTURE}/root.xr ${XIR_STATE_ONE_FIXTURE}/base.xr
        ${XIR_STATE_ONE_FIXTURE}/left.xr ${XIR_STATE_ONE_FIXTURE}/right.xr
        ${XIR_STATE_ONE_FIXTURE}/reject-private.xr
    VERBATIM)
add_executable(test_xr_program_aot_module_state_1
    xir/test_xir_module_state_one_native.c ${XIR_STATE_ONE_GENERATED})
target_link_libraries(test_xr_program_aot_module_state_1 PRIVATE xray_xir_scalar)
foreach(state_one_target IN ITEMS test_xir_module_state_one_source test_xr_program_aot_module_state_1)
    target_include_directories(${state_one_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
        (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${state_one_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${state_one_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${state_one_target} COMMAND $<TARGET_FILE:${state_one_target}>)
    set_tests_properties(${state_one_target} PROPERTIES
        LABELS "unit;xir;canonical-program;execution;module;ownership;abi" TIMEOUT 300)
endforeach()
set_property(TEST test_xr_program_aot_module_state_1 APPEND PROPERTY LABELS "aot;generated-c;native;task-310")
