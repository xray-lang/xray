# Model and process tests use the current command and process owners.
include("${PROJECT_SOURCE_DIR}/src/app/toolchain/xir_process.cmake")

add_executable(process_spawn_benchmark EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/benchmarks/process/process_spawn_benchmark.c")
target_link_libraries(process_spawn_benchmark PRIVATE xray_toolchain_process)
