add_executable(test_xir_stdlib_catalog ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_catalog.c)
target_include_directories(test_xir_stdlib_catalog PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_stdlib_catalog PRIVATE xray_xir_native_cache)
if(MSVC)
    target_compile_options(test_xir_stdlib_catalog PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_stdlib_catalog PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_stdlib_catalog COMMAND test_xir_stdlib_catalog ${PROJECT_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_stdlib_catalog PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;stdlib;budget")
add_test(NAME test_xir_public_cli_checked_stdlib
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_checked_stdlib.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_checked_stdlib PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "cli;xir;stdlib;native;execution")
