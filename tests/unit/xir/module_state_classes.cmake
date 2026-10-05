# Four-slot class initialization preserves actual identity and allocator releases.
add_executable(test_xir_module_state_classes xir/test_xir_module_state_classes.c)
target_link_libraries(test_xir_module_state_classes PRIVATE xray_xir_vm xray_xir_cgen)
set(state_class_targets test_xir_module_state_classes)
foreach(state_class_scenario IN ITEMS 5 8)
    set(state_class_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_class_${state_class_scenario}.c)
    add_custom_command(OUTPUT ${state_class_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND $<TARGET_FILE:test_xir_module_state_classes> ${state_class_c} ${state_class_scenario}
        DEPENDS test_xir_module_state_classes VERBATIM)
    set(state_class_target test_xr_program_aot_module_state_${state_class_scenario})
    add_executable(${state_class_target} xir/test_xir_module_state_class_native.c ${state_class_c})
    target_compile_definitions(${state_class_target} PRIVATE
        XR_STATE_CLASS_SCENARIO=${state_class_scenario} XR_STATE_CLASS_PREFIX=state_class_${state_class_scenario})
    target_link_libraries(${state_class_target} PRIVATE xray_xir_scalar)
    list(APPEND state_class_targets ${state_class_target})
endforeach()
foreach(state_class_target IN LISTS state_class_targets)
    target_include_directories(${state_class_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${state_class_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${state_class_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${state_class_target} COMMAND $<TARGET_FILE:${state_class_target}>)
    set_tests_properties(${state_class_target} PROPERTIES
        LABELS "unit;xir;canonical-program;execution;module;ownership;abi" TIMEOUT 300)
endforeach()
foreach(state_class_scenario IN ITEMS 5 8)
    set_property(TEST test_xr_program_aot_module_state_${state_class_scenario} APPEND PROPERTY LABELS "aot;generated-c;native;task-310")
endforeach()
