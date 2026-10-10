add_executable(test_source_static_methods_current_vm
    "${CMAKE_CURRENT_LIST_DIR}/test_source_static_methods_current_vm.c")
target_link_libraries(test_source_static_methods_current_vm PRIVATE xray_xir_source_product)
target_include_directories(test_source_static_methods_current_vm PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_static_methods_current_vm PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_static_methods_current_vm PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_static_methods_current_vm PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_static_methods_current_vm COMMAND test_source_static_methods_current_vm
    "${CMAKE_CURRENT_LIST_DIR}/fixtures/static_methods"
    "${CMAKE_CURRENT_LIST_DIR}/fixtures/static_methods/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_static_methods_current_vm PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;class;static-method;ownership;source;vm")
