# Every original positive input remains a positive gate, including unsupported inputs.
add_test(NAME source_product_allocation_original_inputs
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_allocation_original.py"
        --root "${CMAKE_SOURCE_DIR}")
set_tests_properties(source_product_allocation_original_inputs PROPERTIES TIMEOUT 30
    LABELS "unit;xir;source-product;program-consumer;source-allocation;input-preservation")
add_executable(test_source_product_allocation_original
    "${CMAKE_CURRENT_LIST_DIR}/test_source_product_allocation_original.c")
target_link_libraries(test_source_product_allocation_original PRIVATE xray_xir_source_product)
target_compile_definitions(test_source_product_allocation_original PRIVATE XR_ALLOCATION_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
target_include_directories(test_source_product_allocation_original PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_allocation_original PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_allocation_original PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_allocation_original PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_allocation_original PRIVATE -Wall -Wextra -Werror)
endif()
foreach(scenario RANGE 0 7)
    set(original_root "${CMAKE_CURRENT_LIST_DIR}/allocation_original/scenario${scenario}")
    add_test(NAME test_source_product_allocation_original_${scenario}_normal
        COMMAND test_source_product_allocation_original "${original_root}" "${original_root}/root.xr" "${scenario}")
    set_tests_properties(test_source_product_allocation_original_${scenario}_normal PROPERTIES
        TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;source-product;program-consumer;source-allocation;ownership;vm-projection")
endforeach()
