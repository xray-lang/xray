# Complete Built programs retain each initialization failure as a sticky outcome.
add_executable(test_xir_module_state_failures xir/test_xir_module_state_failures.c)
target_link_libraries(test_xir_module_state_failures PRIVATE xray_xir_vm xray_xir_cgen)
set(state_failure_targets test_xir_module_state_failures)
foreach(state_failure_scenario RANGE 2 4)
    set(state_failure_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_failure_${state_failure_scenario}.c)
    add_custom_command(OUTPUT ${state_failure_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND $<TARGET_FILE:test_xir_module_state_failures> ${state_failure_c} ${state_failure_scenario}
        DEPENDS test_xir_module_state_failures VERBATIM)
    set(state_failure_target test_xr_program_aot_module_state_${state_failure_scenario})
    add_executable(${state_failure_target} xir/test_xir_module_state_failure_native.c ${state_failure_c})
    target_compile_definitions(${state_failure_target} PRIVATE
        XR_STATE_FAILURE_SCENARIO=${state_failure_scenario}
        XR_STATE_FAILURE_PREFIX=state_failure_${state_failure_scenario})
    target_link_libraries(${state_failure_target} PRIVATE xray_xir_scalar)
    list(APPEND state_failure_targets ${state_failure_target})
endforeach()
foreach(state_failure_target IN LISTS state_failure_targets)
    target_include_directories(${state_failure_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${state_failure_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${state_failure_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${state_failure_target} COMMAND $<TARGET_FILE:${state_failure_target}>)
    set_tests_properties(${state_failure_target} PROPERTIES
        LABELS "unit;xir;canonical-program;execution;module;ownership;abi" TIMEOUT 300)
endforeach()
foreach(state_failure_scenario RANGE 2 4)
    set_property(TEST test_xr_program_aot_module_state_${state_failure_scenario} APPEND PROPERTY LABELS "aot;generated-c;native;task-310")
endforeach()
