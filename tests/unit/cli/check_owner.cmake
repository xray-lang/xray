if(NOT WIN32)
    return()
endif()
foreach(target check_owner_driver check_owner_injected)
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/check_owner/check_owner_driver.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_spec.c"
        "${PROJECT_SOURCE_DIR}/src/app/cli/xcli_diag.c")
    target_link_libraries(${target} PRIVATE xray_cli_source bcrypt)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
target_sources(check_owner_driver PRIVATE "${PROJECT_SOURCE_DIR}/src/app/cli/xcmd_check.c")
target_compile_definitions(check_owner_injected PRIVATE CHECK_OWNER_INJECTED=1)
add_test(NAME check_source_owner COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/check_owner/check_owner_cases.py" --root "${PROJECT_SOURCE_DIR}"
    --driver $<TARGET_FILE:check_owner_driver> --injected $<TARGET_FILE:check_owner_injected>
    --output "${CMAKE_CURRENT_BINARY_DIR}/check-owner")
set_tests_properties(check_source_owner PROPERTIES TIMEOUT 120 COST 3
    LABELS "unit;cli;xir;ownership;budget")
