include_guard(GLOBAL)
add_library(xray_frontend_format STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xfmt.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_expr.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_stmt.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_decl.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_type.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_trivia.c
    ${CMAKE_CURRENT_LIST_DIR}/xfmt_literal.c)
target_link_libraries(xray_frontend_format PUBLIC xray_compiler_parser)
target_compile_features(xray_frontend_format PUBLIC c_std_11)
set_target_properties(xray_frontend_format PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_definitions(xray_frontend_format PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(xray_frontend_format PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(xray_frontend_format PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(xray_frontend_format PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
