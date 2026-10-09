# Canonical typed-error edges retain complete facts and owned failure cleanup.
add_executable(test_xir_error_edge_snapshot xir/test_xir_error_edge_snapshot.c)
target_link_libraries(test_xir_error_edge_snapshot PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_error_edge_snapshot PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_error_edge_snapshot PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_error_edge_snapshot COMMAND test_xir_error_edge_snapshot)
set_tests_properties(test_xir_error_edge_snapshot PROPERTIES
    LABELS "unit;xir;metadata;ownership;budget" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
