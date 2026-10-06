add_executable(test_xir_native_cache ${CMAKE_CURRENT_LIST_DIR}/test_xir_native_cache.c)
target_include_directories(test_xir_native_cache PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_native_cache PRIVATE xray_xir_source xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_native_cache PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_native_cache PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_native_cache
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/native_cache_test.py
        $<TARGET_FILE:test_xir_native_cache> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_native_cache PROPERTIES TIMEOUT 60 LABELS "unit;xir;ownership;native-cache")
