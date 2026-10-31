add_executable(test_xir_tuple_assignment xir/test_xir_tuple_assignment.c)
target_link_libraries(test_xir_tuple_assignment PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(tuple_assignment_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/tuple_assignment.c")
set(tuple_demo_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/tuple_demo.c")
add_custom_command(OUTPUT "${tuple_assignment_generated}" "${tuple_demo_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_tuple_assignment> "${PROJECT_SOURCE_DIR}/demos/01-basics/tuples.xr"
        "${tuple_assignment_generated}" "${tuple_demo_generated}"
    DEPENDS test_xir_tuple_assignment "${PROJECT_SOURCE_DIR}/demos/01-basics/tuples.xr" VERBATIM)
add_executable(test_xir_tuple_assignment_native xir/test_xir_tuple_assignment_native.c
    "${tuple_assignment_generated}" "${tuple_demo_generated}")
target_link_libraries(test_xir_tuple_assignment_native PRIVATE xray_xir_scalar)
foreach(target test_xir_tuple_assignment test_xir_tuple_assignment_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_tuple_assignment COMMAND test_xir_tuple_assignment "${PROJECT_SOURCE_DIR}/demos/01-basics/tuples.xr")
add_test(NAME xir_tuple_assignment_native COMMAND test_xir_tuple_assignment_native)
set_tests_properties(xir_tuple_assignment xir_tuple_assignment_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership;generated-c")
