add_executable(test_source_lowered_correspondence test_lowered_correspondence.c)
target_link_libraries(test_source_lowered_correspondence PRIVATE xray_source_api)
target_include_directories(test_source_lowered_correspondence PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests/unit")
target_compile_definitions(test_source_lowered_correspondence PRIVATE
    XR_CORRESPONDENCE_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(test_source_lowered_correspondence PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_lowered_correspondence PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_lowered_correspondence PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME source_lowered_correspondence COMMAND test_source_lowered_correspondence)
set_tests_properties(source_lowered_correspondence PROPERTIES
    TIMEOUT 60 LABELS "unit;api;source;ownership;resources;lowering")
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
