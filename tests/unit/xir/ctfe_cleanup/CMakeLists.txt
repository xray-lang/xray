add_executable(test_xir_ctfe_cleanup test_ctfe_cleanup.c)
target_link_libraries(test_xir_ctfe_cleanup PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_ctfe_cleanup PRIVATE
    XR_CTFE_CLEANUP_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/cleanup_owned.xr"
    XR_CTFE_CLEANUP_ORIGINAL="${PROJECT_SOURCE_DIR}/tests/unit/xir/p11_ctfe_reference/fixtures/panic_cleanup_escape.xr"
    XR_CTFE_CLEANUP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_ctfe_cleanup PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_cleanup PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_ctfe_cleanup PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_ctfe_cleanup COMMAND test_xir_ctfe_cleanup)
set_tests_properties(test_xir_ctfe_cleanup PROPERTIES TIMEOUT 120 LABELS "unit;xir;ctfe;cleanup;ownership;resources")
add_test(NAME source_ctfe_cleanup_lifetime COMMAND test_source_request_lifetime --ctfe-cleanup)
set_tests_properties(source_ctfe_cleanup_lifetime PROPERTIES TIMEOUT 120
    LABELS "unit;api;source;ctfe;cleanup;ownership")
if(WIN32)
    add_test(NAME test_xir_ctfe_cleanup_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/test_ctfe_cleanup_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(test_xir_ctfe_cleanup_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;xir;ctfe;cleanup;cli;native;ownership")
endif()
