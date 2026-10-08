add_executable(test_source_product_process_lifecycle "${CMAKE_CURRENT_LIST_DIR}/test_source_product_process_lifecycle.c")
target_link_libraries(test_source_product_process_lifecycle PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_process_lifecycle PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_process_lifecycle PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_process_lifecycle PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_process_lifecycle PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_product_process_lifecycle
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_process_lifecycle.py"
    --binary "$<TARGET_FILE:test_source_product_process_lifecycle>"
    --root "${CMAKE_CURRENT_LIST_DIR}/parent_source/open_root_empty" --stdlib "${CMAKE_SOURCE_DIR}/stdlib"
    --input-root "${CMAKE_SOURCE_DIR}"
    --source-file "${CMAKE_CURRENT_LIST_DIR}/test_source_product_process_lifecycle.c"
    --registration-file "${CMAKE_CURRENT_LIST_DIR}/source_product_process_lifecycle.cmake"
    --evidence-root "${CMAKE_CURRENT_BINARY_DIR}/process-lifecycle-evidence")
set_tests_properties(source_product_process_lifecycle PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;process-lifecycle")
