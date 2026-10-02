get_filename_component(XR_PARSER_OWNER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
file(GLOB XR_PARSER_OWNER_SOURCES "${XR_PARSER_OWNER_ROOT}/src/frontend/parser/*.c")
add_executable(test_parser_resources
    "${CMAKE_CURRENT_LIST_DIR}/test_parser_resources.c"
    ${XR_PARSER_OWNER_SOURCES}
    "${XR_PARSER_OWNER_ROOT}/src/frontend/lexer/xlex.c"
    "${XR_PARSER_OWNER_ROOT}/src/frontend/lexer/xquoted_literal.c"
    "${XR_PARSER_OWNER_ROOT}/src/base/xcompile_state.c"
    "${XR_PARSER_OWNER_ROOT}/src/os/win/fd_win.c"
    "${XR_PARSER_OWNER_ROOT}/src/base/xarena.c"
    "${XR_PARSER_OWNER_ROOT}/src/base/xutf8.c"
    "${XR_PARSER_OWNER_ROOT}/src/base/xsimd.c"
    "${XR_PARSER_OWNER_ROOT}/src/toolchain/xcompiler_arena_backing.c"
    "${XR_PARSER_OWNER_ROOT}/src/toolchain/xcompiler_session.c")
target_include_directories(test_parser_resources PRIVATE "${XR_PARSER_OWNER_ROOT}/src" "${XR_PARSER_OWNER_ROOT}/src/base" "${XR_PARSER_OWNER_ROOT}/include")
target_compile_features(test_parser_resources PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_parser_resources PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_parser_resources PRIVATE /W4 /WX /utf-8 /Gy)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_parser_resources PRIVATE /experimental:c11atomics)
    endif()
endif()
add_test(NAME test_parser_resources COMMAND test_parser_resources)
