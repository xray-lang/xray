add_executable(test_xir_p09_hash_sendable "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_sendable.c")
target_link_libraries(test_xir_p09_hash_sendable PRIVATE xray_xir_source  )
target_compile_definitions(test_xir_p09_hash_sendable PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_hash_sendable PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_sendable PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_sendable PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_sendable COMMAND test_xir_p09_hash_sendable)
set_tests_properties(test_xir_p09_hash_sendable PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;sendable;ownership;target-impact")



set(HASH_WORKER_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_worker.c")
