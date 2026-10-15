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
if(WIN32)
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
    add_executable(test_cli_toolchain_update_matrix "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update_matrix.c")
    target_link_libraries(test_cli_toolchain_update_matrix PRIVATE xray_cli_toolchain_update)
    target_compile_definitions(test_cli_toolchain_update_matrix PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(test_cli_toolchain_update_matrix PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(test_cli_toolchain_update_matrix PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_cli_toolchain_update_matrix PRIVATE /experimental:c11atomics)
    endif()
    foreach(mode normal resources)
        set(matrix_args)
        if(mode STREQUAL "resources")
            list(APPEND matrix_args --resources)
        endif()
        add_test(NAME cli_toolchain_update_matrix_${mode} COMMAND "${XRAY_PYTHON}" -X utf8
            "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_update_matrix.py" $<TARGET_FILE:test_cli_toolchain_update_matrix>
            "${CMAKE_CURRENT_BINARY_DIR}/toolchain-update-matrix-${mode}" ${matrix_args})
        set_tests_properties(cli_toolchain_update_matrix_${mode} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
            LABELS "unit;cli;toolchain;ownership;budget")
        if(mode STREQUAL "resources")
            set_tests_properties(cli_toolchain_update_matrix_${mode} PROPERTIES TIMEOUT 600 COST 120)
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
        ENVIRONMENT "XRAY_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib"
        LABELS "unit;cli;toolchain;generated-c")
    add_test(NAME cli_toolchain_probe COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_probe.py" $<TARGET_FILE:xray>
        "${XIR_INVOCATION_MSVC}" "${CMAKE_CURRENT_BINARY_DIR}/toolchain-probe")
    set_tests_properties(cli_toolchain_probe PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "XRAY_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib"
        LABELS "unit;cli;toolchain;sdk;generated-c;ownership")
    add_test(NAME cli_toolchain_preferences COMMAND "${XRAY_PYTHON}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_cli_toolchain_preferences.py" $<TARGET_FILE:xray>
        "${CMAKE_CURRENT_BINARY_DIR}/toolchain-preferences")
    set_tests_properties(cli_toolchain_preferences PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
        LABELS "unit;cli;toolchain;ownership")
endif()
