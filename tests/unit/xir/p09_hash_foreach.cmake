

set(HASH_FOREACH_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_foreach.c")

add_executable(test_xir_p09_hash_position "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_position.c")
target_include_directories(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_p09_hash_position PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_p09_hash_position PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_p09_hash_position PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_position PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_position PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_position COMMAND test_xir_p09_hash_position)
set_tests_properties(test_xir_p09_hash_position PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;ownership;memory;budget;target-impact")
add_test(NAME test_xir_p09_hash_position_allocations COMMAND test_xir_p09_hash_position --full)
set_tests_properties(test_xir_p09_hash_position_allocations PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;hash;ownership;memory;budget;fault")





add_test(NAME test_xir_p09_hash_position81_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_hash_position81_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-hash-position81-vectors")
set_tests_properties(test_xir_p09_hash_position81_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")
