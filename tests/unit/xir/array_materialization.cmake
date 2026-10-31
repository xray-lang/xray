add_executable(test_xir_array_materialization xir/test_xir_array_materialization.c)
target_link_libraries(test_xir_array_materialization PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(array_materialization_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/array_materialization.c")
set(array_demo_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/array_demo.c")
add_custom_command(OUTPUT "${array_materialization_generated}" "${array_demo_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_materialization> "${PROJECT_SOURCE_DIR}/demos/02-collections/arrays.xr"
        "${array_materialization_generated}" "${array_demo_generated}"
    DEPENDS test_xir_array_materialization "${PROJECT_SOURCE_DIR}/demos/02-collections/arrays.xr" VERBATIM)
add_executable(test_xir_array_materialization_native xir/test_xir_array_materialization_native.c
    "${array_materialization_generated}" "${array_demo_generated}")
target_link_libraries(test_xir_array_materialization_native PRIVATE xray_xir_scalar)
foreach(target test_xir_array_materialization test_xir_array_materialization_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_array_materialization COMMAND test_xir_array_materialization "${PROJECT_SOURCE_DIR}/demos/02-collections/arrays.xr")
add_test(NAME xir_array_materialization_native COMMAND test_xir_array_materialization_native)
set_tests_properties(xir_array_materialization xir_array_materialization_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership;generated-c")
