include_guard(GLOBAL)
add_executable(test_xir_mixed_callback_boundary_producer xir/test_xir_mixed_callback_boundary_producer.c)
target_link_libraries(test_xir_mixed_callback_boundary_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_callback_boundary_producer PRIVATE
    XR_CALLBACK_BOUNDARY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary")
set(XIR_MIXED_CALLBACK_BOUNDARY_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_callback_boundary.c")
add_custom_command(OUTPUT "${XIR_MIXED_CALLBACK_BOUNDARY_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_callback_boundary_producer> "${XIR_MIXED_CALLBACK_BOUNDARY_C}"
    DEPENDS test_xir_mixed_callback_boundary_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary/worker.xr"
    VERBATIM)
add_custom_target(test_xir_mixed_callback_boundary_source DEPENDS "${XIR_MIXED_CALLBACK_BOUNDARY_C}")

foreach(callback_boundary_target IN ITEMS test_xir_mixed_callback_boundary_producer)
    set_target_properties(${callback_boundary_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${callback_boundary_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${callback_boundary_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
