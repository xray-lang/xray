add_executable(native_bench_reference ${CMAKE_CURRENT_SOURCE_DIR}/bench/native/reference.c)
target_include_directories(native_bench_reference PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(native_bench_reference PRIVATE
    XR_BENCH_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
add_dependencies(native_bench_reference xray-build-identity-refresh)

add_test(NAME native_bench_runner
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/bench/native/test_runner.py
        --reference $<TARGET_FILE:native_bench_reference>
        --output ${CMAKE_BINARY_DIR}/native-bench-runner-test)
set_tests_properties(native_bench_runner PROPERTIES LABELS "unit;fast" TIMEOUT 30)

# Performance runs are opt-in; the ordinary non-sanitizer suite stays L1-only.
option(XRAY_REGISTER_BENCHMARKS "Register the exclusive performance CTest lane" OFF)
if(XRAY_REGISTER_BENCHMARKS)
add_test(NAME native_bench_reference
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/bench/native/run.py
        --reference $<TARGET_FILE:native_bench_reference>
        --output ${CMAKE_BINARY_DIR}/native-bench-reference)
set_tests_properties(native_bench_reference PROPERTIES LABELS "bench" RUN_SERIAL TRUE TIMEOUT 2000)

add_test(NAME native_bench_current_vm
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/bench/native/run.py
        --reference $<TARGET_FILE:native_bench_reference>
        --xray $<TARGET_FILE:xray>
        --output ${CMAKE_BINARY_DIR}/native-bench-current-vm)
set_tests_properties(native_bench_current_vm PROPERTIES LABELS "bench" RUN_SERIAL TRUE TIMEOUT 4000)
endif()
