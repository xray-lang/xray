add_executable(test_xir_root_go_consumption ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_go_consumption.c)
target_link_libraries(test_xir_root_go_consumption PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_root_go_consumption PRIVATE
    XR_ROOT_QUERY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_effects"
    XR_ROOT_GO_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_go_consumption")
if(MSVC)
    target_compile_options(test_xir_root_go_consumption PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_go_consumption PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_root_go_consumption COMMAND test_xir_root_go_consumption)
set_tests_properties(test_xir_root_go_consumption PROPERTIES
    LABELS "unit;xir;metadata;ownership;budget;memory;root-effects;task" RUN_SERIAL TRUE TIMEOUT 300)
