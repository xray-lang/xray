add_executable(test_xir_array_binding xir/test_xir_array_binding.c)
target_link_libraries(test_xir_array_binding PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(array_binding_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/array_binding.c")
set(array_variables_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/array_variables.c")
add_custom_command(OUTPUT "${array_binding_generated}" "${array_variables_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_binding> "${PROJECT_SOURCE_DIR}/demos/01-basics/variables.xr"
        "${array_binding_generated}" "${array_variables_generated}"
    DEPENDS test_xir_array_binding "${PROJECT_SOURCE_DIR}/demos/01-basics/variables.xr" VERBATIM)
add_executable(test_xir_array_binding_native xir/test_xir_array_binding_native.c
    "${array_binding_generated}" "${array_variables_generated}")
target_link_libraries(test_xir_array_binding_native PRIVATE xray_xir_scalar)
foreach(target test_xir_array_binding test_xir_array_binding_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_array_binding COMMAND test_xir_array_binding "${PROJECT_SOURCE_DIR}/demos/01-basics/variables.xr")
add_test(NAME xir_array_binding_native COMMAND test_xir_array_binding_native)
set_tests_properties(xir_array_binding xir_array_binding_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership;generated-c")
