/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_codes.h - Error codes (E01xx=Lexer, E02xx=Syntax, E03xx=Compile, E04xx=Runtime, E05xx=Module)
 *
 * KEY CONCEPT:
 *   Centralized error code definitions with auto-generated hints.
 *   Codes follow Exxxx format for easy search and documentation.
 */

#ifndef XERROR_CODES_H
#define XERROR_CODES_H

// Lexer errors (E01xx)
#define XR_ERR_LEX_INVALID_CHAR     101
#define XR_ERR_LEX_UNTERMINATED_STR 102
#define XR_ERR_LEX_INVALID_NUMBER   103
#define XR_ERR_LEX_INVALID_ESCAPE   104

// Syntax errors (E02xx)
#define XR_ERR_SYN_UNEXPECTED_TOKEN 201
#define XR_ERR_SYN_EXPECTED_EXPR    202
#define XR_ERR_SYN_EXPECTED_STMT    203
#define XR_ERR_SYN_UNCLOSED_PAREN   204
#define XR_ERR_SYN_UNCLOSED_BRACE   205
#define XR_ERR_SYN_UNCLOSED_BRACKET 206
#define XR_ERR_SYN_INVALID_ASSIGN   207

// Compile errors (E03xx)
#define XR_ERR_CMP_UNDEFINED_VAR    301
#define XR_ERR_CMP_REDEFINED_VAR    302
#define XR_ERR_CMP_CONST_ASSIGN     303
#define XR_ERR_CMP_INVALID_BREAK    304
#define XR_ERR_CMP_INVALID_CONTINUE 305
#define XR_ERR_CMP_INVALID_RETURN   306
#define XR_ERR_CMP_TOO_MANY_PARAMS  307
#define XR_ERR_CMP_TOO_MANY_LOCALS  308

// Runtime errors (E04xx)
#define XR_ERR_TYPE_NO_PROPERTY     401
#define XR_ERR_TYPE_NO_INDEX        402
#define XR_ERR_TYPE_NO_CALL         403
#define XR_ERR_TYPE_MISMATCH        404
#define XR_ERR_TYPE_NO_METHOD       405
#define XR_ERR_TYPE_NO_OPERATOR     406

#define XR_ERR_NULL_PROPERTY        410
#define XR_ERR_NULL_INDEX           411
#define XR_ERR_NULL_CALL            412

#define XR_ERR_DIV_BY_ZERO          420
#define XR_ERR_MOD_BY_ZERO          421
#define XR_ERR_OVERFLOW             422

#define XR_ERR_INDEX_OUT_OF_BOUNDS  430
#define XR_ERR_KEY_NOT_FOUND        431

#define XR_ERR_STACK_OVERFLOW       440
#define XR_ERR_OUT_OF_MEMORY        441

#define XR_ERR_WRONG_ARG_COUNT      450
#define XR_ERR_INVALID_ARG_TYPE     451

#define XR_ERR_CORO_DEAD            460
#define XR_ERR_CORO_CANCELLED       461

// Module errors (E05xx)
#define XR_ERR_MOD_NOT_FOUND        501
#define XR_ERR_MOD_LOAD_FAILED      502
#define XR_ERR_MOD_NO_EXPORT        503
#define XR_ERR_MOD_CIRCULAR         504

#endif // XERROR_CODES_H
