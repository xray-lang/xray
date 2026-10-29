add_executable(test_xir_local_errors "${CMAKE_CURRENT_LIST_DIR}/test_xir_local_errors.c")
target_link_libraries(test_xir_local_errors PRIVATE xray_xir_source_product xray_xir_vm)
target_compile_definitions(test_xir_local_errors PRIVATE XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_local_errors PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_local_errors PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_local_errors COMMAND test_xir_local_errors)
set_tests_properties(test_xir_local_errors PROPERTIES LABELS "unit;xir;language;ownership;execution" TIMEOUT 60)
if(WIN32)
    add_test(NAME test_xir_initialization_cycles_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_xir_initialization_cycles_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(test_xir_initialization_cycles_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;xir;errors;cli;native")
endif()
