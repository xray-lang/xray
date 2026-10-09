add_executable(test_xir_callable_root_source ${CMAKE_CURRENT_LIST_DIR}/test_xir_callable_root_source.c)
target_include_directories(test_xir_callable_root_source PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_callable_root_source PRIVATE xray_xir_source)
target_compile_definitions(test_xir_callable_root_source PRIVATE
    XR_CALLABLE_ROOT_SOURCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_callable_root_source")
if(MSVC)
    target_compile_options(test_xir_callable_root_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_callable_root_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_callable_root_source COMMAND test_xir_callable_root_source)
set_tests_properties(test_xir_callable_root_source PROPERTIES
    LABELS "unit;xir;source;root-effects;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_callable_root_source_compiler COMMAND test_xir_callable_root_source --compiler)
set_tests_properties(test_xir_callable_root_source_compiler PROPERTIES
    LABELS "unit;xir;source;root-effects;compiler;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
