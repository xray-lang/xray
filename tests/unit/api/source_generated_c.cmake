add_executable(test_source_generated_c "${CMAKE_CURRENT_LIST_DIR}/test_source_generated_c.c")
target_link_libraries(test_source_generated_c PRIVATE xray_source_compile xray_xir_native_projection)
target_include_directories(test_source_generated_c PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit")
set_target_properties(test_source_generated_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_generated_c PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_generated_c PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME source_native_projection_ownership COMMAND test_source_generated_c
    "${PROJECT_SOURCE_DIR}/demos/01-basics/control_flow.xr"
    "${CMAKE_CURRENT_BINARY_DIR}/source-native-projection-ownership.c"
    "${PROJECT_SOURCE_DIR}/stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/source-native-projection-ownership.json" --guards)
set_tests_properties(source_native_projection_ownership PROPERTIES
    TIMEOUT 120 LABELS "unit;api;source;ownership;generated-c")
