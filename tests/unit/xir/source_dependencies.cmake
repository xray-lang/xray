add_executable(test_xir_dependency_snapshot xir/test_xir_dependency_snapshot.c)
target_link_libraries(test_xir_dependency_snapshot PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_dependency_snapshot PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_dependency_snapshot PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_dependency_snapshot COMMAND test_xir_dependency_snapshot)
set_tests_properties(test_xir_dependency_snapshot PROPERTIES TIMEOUT 120 LABELS "unit;xir;ownership;budget")
add_executable(test_xir_source_dependencies xir/test_xir_source_dependencies.c)
target_link_libraries(test_xir_source_dependencies PRIVATE xray_cli_source xray_xir_native_cache)
if(MSVC)
    target_compile_options(test_xir_source_dependencies PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_dependencies PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_dependencies COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_dependencies.py
    $<TARGET_FILE:test_xir_source_dependencies> ${CMAKE_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_source_dependencies PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "unit;xir;cli;ownership;budget")

add_test(NAME test_cli_source_dependencies COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_cli_dependencies.py
    $<TARGET_FILE:xray> ${CMAKE_SOURCE_DIR}/stdlib)
set_tests_properties(test_cli_source_dependencies PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "unit;cli;xir;ownership")

add_executable(test_cli_dependency_output xir/test_cli_dependency_output.c)
target_link_libraries(test_cli_dependency_output PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_cli_dependency_output PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_cli_dependency_output PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_cli_dependency_output COMMAND test_cli_dependency_output)
set_tests_properties(test_cli_dependency_output PROPERTIES TIMEOUT 120 LABELS "unit;cli;xir;ownership")
