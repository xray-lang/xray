# Model and process tests use the current command and process owners.
include("${PROJECT_SOURCE_DIR}/src/app/toolchain/xir_process.cmake")
add_executable(test_cli_toolchain "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain.c"
    "${PROJECT_SOURCE_DIR}/src/app/toolchain/xtc_command.c"
    "${PROJECT_SOURCE_DIR}/src/app/toolchain/xtc_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/toolchain/xtc_json.c"
    "${PROJECT_SOURCE_DIR}/src/app/toolchain/xtc_probe_cache.c"
    "${PROJECT_SOURCE_DIR}/src/app/toolchain/xtc_discovery.c"
    "${PROJECT_SOURCE_DIR}/src/base/xjson.c")
target_include_directories(test_cli_toolchain PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_cli_toolchain PRIVATE xray_cli_toolchain_config xray_toolchain_process)
add_test(NAME test_cli_toolchain COMMAND test_cli_toolchain)

add_executable(process_spawn_benchmark EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/benchmarks/process/process_spawn_benchmark.c")
target_link_libraries(process_spawn_benchmark PRIVATE xray_toolchain_process)
