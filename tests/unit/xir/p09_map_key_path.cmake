add_executable(test_xir_p09_map_key_path_value "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_key_path_value.c")
target_include_directories(test_xir_p09_map_key_path_value PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_p09_map_key_path_value PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_p09_map_key_path_value PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_p09_map_key_path_value PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_p09_map_key_path_value PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_p09_map_key_path_value PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_map_key_path_value PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_map_key_path_value PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_map_key_path_value COMMAND test_xir_p09_map_key_path_value)
set_tests_properties(test_xir_p09_map_key_path_value PROPERTIES TIMEOUT 120 LABELS "unit;xir;map;path;ownership;memory;budget")
add_test(NAME test_xir_p09_map_key_path_value_allocations COMMAND test_xir_p09_map_key_path_value --full)
set_tests_properties(test_xir_p09_map_key_path_value_allocations PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;map;path;ownership;memory;budget;fault")








set(MAP_KEY_PATH_C "${CMAKE_BINARY_DIR}/generated/xir_p09_map_key_path.c")













add_test(NAME test_xir_p09_map_key_path83_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_map_key_path83_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-map-key-path83-vectors")
set_tests_properties(test_xir_p09_map_key_path83_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;map;path;codec")



set(MAP_KEY_BRIDGE_C "${CMAKE_BINARY_DIR}/generated/xir_p09_map_key_bridge.c")
