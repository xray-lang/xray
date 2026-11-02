add_executable(test_xir_class_dispatch_runtime xir/test_xir_class_dispatch_runtime.c)
target_link_libraries(test_xir_class_dispatch_runtime PRIVATE
    xray_xir_scalar xray_xir xray_xir_source xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_class_dispatch_runtime PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_class_dispatch_runtime PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_class_dispatch_runtime COMMAND test_xir_class_dispatch_runtime)
set_tests_properties(test_xir_class_dispatch_runtime PROPERTIES
    LABELS "unit;xir;execution;ownership;resources" TIMEOUT 60)
