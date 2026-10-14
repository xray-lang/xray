add_executable(test_xir_source_product_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_product_vm.c)
target_include_directories(test_xir_source_product_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_product_vm PRIVATE xray_xir_source_product xray_xir_stdlib_catalog)
if(MSVC)
    target_compile_options(test_xir_source_product_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_source_product_vm PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_product_vm
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/source_product_vm_test.py
        $<TARGET_FILE:test_xir_source_product_vm> ${PROJECT_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_source_product_vm PROPERTIES TIMEOUT 60 LABELS "unit;xir;ownership;stdlib;execution")
