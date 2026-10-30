if(NOT WIN32)
    return()
endif()
add_executable(test_native_ready test_native_ready.c)
target_link_libraries(test_native_ready PRIVATE xray_xir_local_toolchain)
target_include_directories(test_native_ready PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(test_native_ready PRIVATE NDEBUG WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_target_properties(test_native_ready PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_ready PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_ready PRIVATE /experimental:c11atomics)
    endif()
endif()
if(TARGET xir-runtime-sdk-bundle)
    add_dependencies(test_native_ready xir-runtime-sdk-bundle)
endif()
find_program(NATIVE_READY_MSVC NAMES cl REQUIRED)
add_test(NAME native_ready_owner COMMAND test_native_ready)
add_test(NAME native_ready_discovery COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/test_native_ready.py" --exe "$<TARGET_FILE:test_native_ready>"
    --sdk "${CMAKE_BINARY_DIR}/xir-runtime-sdk" --compiler "${NATIVE_READY_MSVC}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/discovery")
add_test(NAME native_ready_cli COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/test_native_ready_cli.py" --binary "$<TARGET_FILE:xray>"
    --root "${PROJECT_SOURCE_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/cli")
set_tests_properties(native_ready_owner PROPERTIES TIMEOUT 120 LABELS "unit;cli;toolchain;ownership;resources")
set_tests_properties(native_ready_discovery PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE COST 35 LABELS "unit;cli;toolchain;ownership;resources")
set_tests_properties(native_ready_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;cli;toolchain;native;environment")

add_test(NAME native_ready_cold_cli COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/test_native_ready_cli.py" --binary "$<TARGET_FILE:xray>"
    --root "${PROJECT_SOURCE_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/cold-cli" --cold)
add_test(NAME native_ready_probe COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/../test_cli_toolchain_probe.py" "$<TARGET_FILE:xray>"
    "${NATIVE_READY_MSVC}" "${CMAKE_CURRENT_BINARY_DIR}/probe")
set_tests_properties(native_ready_cold_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;cli;toolchain;native;environment")
set_tests_properties(native_ready_probe PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 15 LABELS "unit;cli;toolchain;native;ownership;runtime-sdk")
