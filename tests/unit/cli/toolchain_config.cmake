add_executable(test_cli_toolchain_config "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_config.c")
target_link_libraries(test_cli_toolchain_config PRIVATE xray_cli_toolchain_config)
target_compile_definitions(test_cli_toolchain_config PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(test_cli_toolchain_config PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_cli_toolchain_config PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_cli_toolchain_config PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME cli_toolchain_config COMMAND "${XRAY_PYTHON}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_config.py" $<TARGET_FILE:test_cli_toolchain_config>
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain-config")
set_tests_properties(cli_toolchain_config PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "unit;cli;toolchain;ownership;budget")
add_test(NAME cli_toolchain_config_normal COMMAND "${XRAY_PYTHON}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_config.py" $<TARGET_FILE:test_cli_toolchain_config>
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain-config-normal" --normal)
set_tests_properties(cli_toolchain_config_normal PROPERTIES TIMEOUT 120
    LABELS "unit;cli;toolchain;ownership")
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "MSVC"
    AND NOT ENABLE_ASAN AND NOT ENABLE_SANITIZERS AND NOT ENABLE_UBSAN
    AND NOT ENABLE_TSAN AND NOT ENABLE_MSAN)
    add_test(NAME cli_toolchain_probe_repaired COMMAND ${XRAY_PYTHON} -X utf8 -B
        "${PROJECT_SOURCE_DIR}/tests/unit/cli/test_cli_toolchain_probe.py"
        $<TARGET_FILE:xray> "${CMAKE_C_COMPILER}"
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-probe-repaired")
    set_tests_properties(cli_toolchain_probe_repaired PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 10
        LABELS "unit;cli;source;sdk;native;probe"
        ENVIRONMENT "XRAY_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib")
endif()
if(WIN32)
    add_executable(test_cli_toolchain_command "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_command.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcmd_toolchain.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_spec.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_diag.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_output.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    target_link_libraries(test_cli_toolchain_command PRIVATE xray_cli_toolchain_update )
    target_compile_definitions(test_cli_toolchain_command PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(test_cli_toolchain_command PRIVATE "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated")
    set_target_properties(test_cli_toolchain_command PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(test_cli_toolchain_command PRIVATE /W4 /WX /utf-8)
    foreach(mode normal resources)
        set(command_args)
        if(mode STREQUAL "resources")
            list(APPEND command_args --resources)
        endif()
        add_test(NAME cli_toolchain_command_${mode} COMMAND "${XRAY_PYTHON}" -X utf8
            "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_command.py" $<TARGET_FILE:test_cli_toolchain_command>
            "${CMAKE_CURRENT_BINARY_DIR}/toolchain-command-${mode}" ${command_args})
        set_tests_properties(cli_toolchain_command_${mode} PROPERTIES TIMEOUT 120
            LABELS "unit;cli;toolchain;ownership")
        if(mode STREQUAL "resources")
            set_tests_properties(cli_toolchain_command_${mode} PROPERTIES TIMEOUT 600 COST 30 RUN_SERIAL TRUE
                LABELS "unit;cli;toolchain;ownership;compiler-faults")
        endif()
    endforeach()
    add_test(NAME cli_toolchain_public COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_public.py" $<TARGET_FILE:xray>
        "${XIR_INVOCATION_MSVC}" "${CMAKE_CURRENT_BINARY_DIR}/toolchain-public")
    set_tests_properties(cli_toolchain_public PROPERTIES TIMEOUT 300
        LABELS "unit;cli;toolchain;ownership;generated-c")
    foreach(target test_cli_toolchain_update test_cli_toolchain_update_injected)
        add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update.c")
        target_link_libraries(${target} PRIVATE xray_cli_toolchain_update)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    endforeach()
    target_compile_definitions(test_cli_toolchain_update_injected PRIVATE UPDATE_INJECTED)

    foreach(mode normal resources)
        set(matrix_args)
        if(mode STREQUAL "resources")
            list(APPEND matrix_args --resources)
        endif()

    endforeach()
    add_test(NAME cli_toolchain_update_cleanup COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update_cleanup.py" $<TARGET_FILE:test_cli_toolchain_update_injected>
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-update-cleanup")
    set_tests_properties(cli_toolchain_update_cleanup PROPERTIES TIMEOUT 120
        LABELS "unit;cli;toolchain;ownership")
    add_test(NAME cli_toolchain_update COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update.py" $<TARGET_FILE:test_cli_toolchain_update>
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-update")
    add_test(NAME cli_toolchain_state COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_state.py" $<TARGET_FILE:test_cli_toolchain_update>
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-state")
    set_tests_properties(cli_toolchain_state PROPERTIES TIMEOUT 120
        LABELS "unit;cli;toolchain;ownership;budget")
    add_test(NAME cli_toolchain_update_resources COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update.py" $<TARGET_FILE:test_cli_toolchain_update>
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-update-resources" --resources)
    set_tests_properties(cli_toolchain_update PROPERTIES TIMEOUT 120
        LABELS "unit;cli;toolchain;ownership")
    set_tests_properties(cli_toolchain_update_resources PROPERTIES TIMEOUT 600 COST 60 RUN_SERIAL TRUE
        LABELS "unit;cli;toolchain;ownership;budget;compiler-faults")
    add_test(NAME cli_toolchain_config_native COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_config_native.py" $<TARGET_FILE:xray>
        "${XIR_INVOCATION_MSVC}" "${CMAKE_CURRENT_BINARY_DIR}/toolchain-config-native")
    set_tests_properties(cli_toolchain_config_native PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        LABELS "unit;cli;toolchain;generated-c")
endif()
