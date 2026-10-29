add_executable(test_source_global_callable test_global_callable.c)
target_link_libraries(test_source_global_callable PRIVATE xray_xir_source)
set_target_properties(test_source_global_callable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_global_callable PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_global_callable PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME source_global_callable COMMAND test_source_global_callable)
set_tests_properties(source_global_callable PROPERTIES TIMEOUT 60 LABELS "unit;xir;source;effects;ownership")
add_test(NAME source_global_callable_sdk COMMAND test_source_sdk_consumer --global-callable
    "${PROJECT_BINARY_DIR}/tests/unit/source-sdk-empty-stdlib")
set_tests_properties(source_global_callable_sdk PROPERTIES TIMEOUT 120 LABELS "unit;api;sdk;source;ownership")
if(WIN32)
    add_test(NAME source_global_callable_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/test_global_callable_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(source_global_callable_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;source;effects;cli;native")
endif()
