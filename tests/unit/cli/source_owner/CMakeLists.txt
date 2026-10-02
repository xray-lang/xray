cmake_minimum_required(VERSION 3.25)
project(XirCliSourceOwner C)
if(NOT WIN32 OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "CLI Source qualification requires Windows and Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(CLI_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT IS_ABSOLUTE "${CLI_SOURCE_PRODUCTION_BUILD}")
    message(FATAL_ERROR "Set CLI_SOURCE_PRODUCTION_BUILD to the freshly built production tree")
endif()
set(CLI_SOURCE_LIBRARIES)
foreach(item xir/xray_xir_source_product xir/xray_xir_source xir/xray_xir_vm
             xir/xray_xir_scalar xir/xray_xir_cgen xir/xray_xir xir/xray_xir_admission
             xir/xray_xir_declarations xir/xray_compiler_parser base/xray_compile_resources)
    get_filename_component(name "${item}" NAME)
    set(path "${CLI_SOURCE_PRODUCTION_BUILD}/src/${item}.lib")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Production archive is missing: ${path}")
    endif()
    add_library(cli_source_${name} STATIC IMPORTED)
    set_target_properties(cli_source_${name} PROPERTIES IMPORTED_LOCATION "${path}")
    list(APPEND CLI_SOURCE_LIBRARIES cli_source_${name})
endforeach()
add_executable(test_cli_source_owner test_cli_source_owner.c
    "${CLI_SOURCE_ROOT}/src/app/cli/xcli_canonical_source.c"
    "${CLI_SOURCE_ROOT}/src/app/cli/xcli_graph_authority.c"
    "${CLI_SOURCE_ROOT}/src/module/xproject.c"
    "${CLI_SOURCE_ROOT}/src/module/xnative_package.c")
target_include_directories(test_cli_source_owner PRIVATE "${CLI_SOURCE_ROOT}/src" "${CLI_SOURCE_ROOT}/include")
target_link_libraries(test_cli_source_owner PRIVATE ${CLI_SOURCE_LIBRARIES} bcrypt)
target_compile_definitions(test_cli_source_owner PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(test_cli_source_owner PRIVATE /W4 /WX /utf-8)
option(CLI_SOURCE_SANITIZE "Instrument the CLI owner and match instrumented production archives" OFF)
if(CLI_SOURCE_SANITIZE)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "CLI Source sanitizers require clang-cl")
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    target_compile_options(test_cli_source_owner PRIVATE /fsanitize=address /Oy-
        /clang:-fsanitize=undefined /clang:-fno-sanitize=function)
    target_link_options(test_cli_source_owner PRIVATE /INCREMENTAL:NO /INCLUDE:__asan_seh_interceptor
        "/WHOLEARCHIVE:${clang_resource}/lib/windows/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    target_link_libraries(test_cli_source_owner PRIVATE "${clang_resource}/lib/windows/clang_rt.asan_dynamic-x86_64.lib")
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_cli_source_owner PRIVATE /experimental:c11atomics)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
enable_testing()
add_test(NAME test_cli_source_owner COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_cli_source_owner.py" $<TARGET_FILE:test_cli_source_owner>
    "${CLI_SOURCE_ROOT}/stdlib")
set_tests_properties(test_cli_source_owner PROPERTIES TIMEOUT 300 COST 40 RUN_SERIAL TRUE
    LABELS "unit;cli;xir;ownership;budget")
