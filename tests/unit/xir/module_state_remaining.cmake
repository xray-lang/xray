# Four-slot class initialization preserves actual identity and allocator releases.
add_executable(test_xir_module_state_remaining xir/test_xir_module_state_remaining.c)
target_link_libraries(test_xir_module_state_remaining PRIVATE xray_xir_vm xray_xir_cgen)
set(state_remaining_targets test_xir_module_state_remaining)
foreach(state_remaining_scenario IN ITEMS 6 7 9 10 11 12 13)
    set(state_remaining_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_remaining_${state_remaining_scenario}.c)
    add_custom_command(OUTPUT ${state_remaining_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND $<TARGET_FILE:test_xir_module_state_remaining> ${state_remaining_c} ${state_remaining_scenario}
        DEPENDS test_xir_module_state_remaining VERBATIM)
    set(state_remaining_target test_xr_program_aot_module_state_${state_remaining_scenario})
    add_executable(${state_remaining_target} xir/test_xir_module_state_remaining_native.c ${state_remaining_c})
    target_compile_definitions(${state_remaining_target} PRIVATE
        XR_STATE_REMAINING_SCENARIO=${state_remaining_scenario} XR_STATE_REMAINING_PREFIX=state_remaining_${state_remaining_scenario})
    target_link_libraries(${state_remaining_target} PRIVATE xray_xir_scalar)
    list(APPEND state_remaining_targets ${state_remaining_target})
endforeach()
foreach(state_remaining_target IN LISTS state_remaining_targets)
    target_include_directories(${state_remaining_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${state_remaining_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${state_remaining_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${state_remaining_target} COMMAND $<TARGET_FILE:${state_remaining_target}>)
    set_tests_properties(${state_remaining_target} PROPERTIES
        LABELS "unit;xir;canonical-program;execution;module;ownership;abi" TIMEOUT 300)
endforeach()
foreach(state_remaining_scenario IN ITEMS 6 7 9 10 11 12 13)
    set_property(TEST test_xr_program_aot_module_state_${state_remaining_scenario} APPEND PROPERTY LABELS "aot;generated-c;native;task-310")
endforeach()
