cmake_minimum_required(VERSION 3.25)
project(XirCliParserOwner C)
if(NOT WIN32 OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This parser gate requires Windows Ninja")
endif()
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT IS_ABSOLUTE "${PARSER_SOURCE_BUILD}" OR NOT EXISTS "${PARSER_SOURCE_BUILD}/generated/xray_build_identity.h")
    message(FATAL_ERROR "Set PARSER_SOURCE_BUILD to the real configured production directory")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(test_cli_parser_owner ../test_cli_spec.c
    "${XR_ROOT}/src/app/cli/xcli_spec.c"
    "${XR_ROOT}/src/app/cli/xcli_parser.c"
    "${XR_ROOT}/src/app/cli/xcli_help.c"
    "${XR_ROOT}/src/app/cli/xcli_diag.c"
    "${XR_ROOT}/src/app/cli/xcli_output.c"
    "${XR_ROOT}/src/os/win/fd_win.c"
    "${XR_ROOT}/src/os/win/time_win.c")
target_include_directories(test_cli_parser_owner PRIVATE
    "${XR_ROOT}/src" "${XR_ROOT}/include" "${PARSER_SOURCE_BUILD}/generated")
target_compile_definitions(test_cli_parser_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(test_cli_parser_owner PRIVATE /W4 /WX /utf-8)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_cli_parser_owner PRIVATE /experimental:c11atomics)
endif()
enable_testing()
add_test(NAME cli_parser_owner COMMAND test_cli_parser_owner)
set_tests_properties(cli_parser_owner PROPERTIES TIMEOUT 30 COST 1)
