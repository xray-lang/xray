add_executable(test_xir_native_cache_resources ${CMAKE_CURRENT_LIST_DIR}/test_xir_native_cache_resources.c)
target_include_directories(test_xir_native_cache_resources PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_native_cache_resources PRIVATE xray_xir_source xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_native_cache_resources PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_native_cache_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode measure faults)
    add_test(NAME test_xir_native_cache_${mode}
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/native_cache_resources.py
            $<TARGET_FILE:test_xir_native_cache_resources> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_native_cache_${mode} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;native-cache;resources")
endforeach()
