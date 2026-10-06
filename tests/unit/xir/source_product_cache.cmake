add_executable(test_xir_source_product_cache ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_product_cache.c)
target_include_directories(test_xir_source_product_cache PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_product_cache PRIVATE xray_xir_source_product)
if(MSVC)
    target_compile_options(test_xir_source_product_cache PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_source_product_cache PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_product_cache
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/source_product_cache_test.py
        $<TARGET_FILE:test_xir_source_product_cache> ${PROJECT_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_source_product_cache PROPERTIES TIMEOUT 60 LABELS "unit;xir;ownership;native-cache;execution")
