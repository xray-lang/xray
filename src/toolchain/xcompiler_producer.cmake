# The parser owns only source syntax and caller-supplied compiler resources.
include_guard(GLOBAL)
get_filename_component(XR_COMPILER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(XR_COMPILER_PARSER_SOURCES
    frontend/parser/xast.c frontend/parser/xast_walk.c frontend/parser/xattribute_registry.c
    frontend/parser/xcompile_type_format.c frontend/parser/xparse.c
    frontend/parser/xparse_coroutine.c frontend/parser/xparse_decl.c
    frontend/parser/xparse_destructure.c frontend/parser/xparse_diagnostics.c
    frontend/parser/xparse_enum.c frontend/parser/xparse_exception.c frontend/parser/xparse_expr.c
    frontend/parser/xparse_import.c frontend/parser/xparse_match.c frontend/parser/xparse_oop.c
    frontend/parser/xparse_owner.c frontend/parser/xparse_resources.c frontend/parser/xparse_stmt.c
    frontend/parser/xparse_type.c frontend/parser/xstring_pool.c frontend/parser/xtype_ref.c
    frontend/parser/xtype_scope.c frontend/lexer/xlex.c frontend/lexer/xquoted_literal.c
    base/xcompile_state.c base/xarena.c base/xutf8.c base/xsimd.c
    toolchain/xcompiler_arena_backing.c toolchain/xcompiler_session.c)
if(WIN32)
    list(APPEND XR_COMPILER_PARSER_SOURCES os/win/fd_win.c)
else()
    list(APPEND XR_COMPILER_PARSER_SOURCES os/unix/fd_unix.c)
endif()
list(TRANSFORM XR_COMPILER_PARSER_SOURCES PREPEND "${XR_COMPILER_ROOT}/src/")
add_library(xray_compiler_parser STATIC ${XR_COMPILER_PARSER_SOURCES})
target_link_libraries(xray_compiler_parser PUBLIC xray_compile_resources)
target_include_directories(xray_compiler_parser PUBLIC "${XR_COMPILER_ROOT}/src" "${XR_COMPILER_ROOT}/include")
target_compile_features(xray_compiler_parser PUBLIC c_std_11)
set_target_properties(xray_compiler_parser PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_definitions(xray_compiler_parser PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(xray_compiler_parser PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(xray_compiler_parser PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(xray_compiler_parser PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
