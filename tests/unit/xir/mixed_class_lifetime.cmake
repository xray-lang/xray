add_executable(test_xir_mixed_class_producer xir/test_xir_mixed_class_producer.c)
target_link_libraries(test_xir_mixed_class_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_class_producer PRIVATE
    XR_CLASS_LIFETIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/unit/xir/fixtures/p20_retained_class")
set_target_properties(test_xir_mixed_class_producer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_class_producer PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_class_producer PRIVATE -Wall -Wextra -Werror)
endif()
set(XIR_CLASS_LIFETIME_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_class_lifetime.c")
add_custom_command(OUTPUT "${XIR_CLASS_LIFETIME_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_class_producer> "${XIR_CLASS_LIFETIME_GENERATED}"
    DEPENDS test_xir_mixed_class_producer
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/fixtures/p20_retained_class/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/fixtures/p20_retained_class/callee.xr"
    VERBATIM)
add_custom_target(test_xir_mixed_class_source DEPENDS "${XIR_CLASS_LIFETIME_GENERATED}")
