add_executable(test_xir_stdlib_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_vm.c)
target_include_directories(test_xir_stdlib_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_stdlib_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_stdlib_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_stdlib_vm PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_stdlib_vm
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/stdlib_vm_test.py
        $<TARGET_FILE:test_xir_stdlib_vm> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_stdlib_vm PROPERTIES TIMEOUT 60 LABELS "unit;xir;ownership;stdlib")
