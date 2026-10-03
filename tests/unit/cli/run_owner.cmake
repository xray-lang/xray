if(NOT WIN32)
    return()
endif()
foreach(target run_owner_driver run_owner_injected)
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/run_owner/run_owner_driver.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_diag.c")
endforeach()
add_executable(run_owner_emit "${PROJECT_SOURCE_DIR}/tests/unit/xir/host_cli_owner/emit_host_cli.c")
foreach(target run_owner_driver run_owner_injected run_owner_emit)
    target_link_libraries(${target} PRIVATE xray_cli_source xray_xir_runtime_host bcrypt)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
target_sources(run_owner_driver PRIVATE "${PROJECT_SOURCE_DIR}/src/app/cli/xcmd_run.c")
target_compile_definitions(run_owner_injected PRIVATE RUN_OWNER_INJECTED=1)
add_test(NAME run_source_owner COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/run_owner/run_owner_cases.py" --root "${PROJECT_SOURCE_DIR}"
    --driver $<TARGET_FILE:run_owner_driver> --injected $<TARGET_FILE:run_owner_injected>
    --output "${CMAKE_CURRENT_BINARY_DIR}/run-owner")
set_tests_properties(run_source_owner PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 2
    LABELS "unit;cli;xir;ownership;budget")
add_test(NAME run_source_generated COMMAND ${XRAY_PYTHON} -X utf8
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/host_cli_owner/source_cases.py" --root "${PROJECT_SOURCE_DIR}"
    --emitter $<TARGET_FILE:run_owner_emit> --output "${CMAKE_CURRENT_BINARY_DIR}/run-generated-cases")
set_tests_properties(run_source_generated PROPERTIES TIMEOUT 90 COST 1
    LABELS "unit;cli;xir;ownership;generated-c")
