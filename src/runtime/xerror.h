/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror.h - Error codes and ANSI color utilities
 *
 * KEY CONCEPT:
 *   XrErrorCode enum for compile-time and analyzer error codes.
 *   Runtime error codes use #define in xerror_codes.h.
 *   XrError is a legacy GC object used by exception conversion.
 *
 * RELATED MODULES:
 *   - xerror_codes.h: Runtime error code definitions (used by VM/JIT)
 *   - xexception.h: Runtime exception objects
 */

#ifndef XERROR_H
#define XERROR_H

#include <stdbool.h>
#include <unistd.h>  // isatty
#include <stdio.h>   // FILE, stderr

/* ========== ANSI Color Codes ========== */

#define XR_COLOR_SUPPORTED() (isatty(fileno(stderr)))
#define XR_COLOR_RESET "\033[0m"
#define XR_COLOR_RED "\033[31m"
#define XR_COLOR_GREEN "\033[32m"
#define XR_COLOR_YELLOW "\033[33m"
#define XR_COLOR_BLUE "\033[34m"
#define XR_COLOR_MAGENTA "\033[35m"
#define XR_COLOR_CYAN "\033[36m"
#define XR_COLOR_WHITE "\033[37m"
#define XR_COLOR_GRAY "\033[90m"

#define XR_COLOR_BOLD "\033[1m"
#define XR_COLOR_BOLD_RED "\033[1;31m"
#define XR_COLOR_BOLD_YELLOW "\033[1;33m"
#define XR_COLOR_BOLD_CYAN "\033[1;36m"

/* ========== Error Codes ========== */

// Error codes used by analyzer and exception system.
// Runtime VM/JIT error codes are in xerror_codes.h (#define).
typedef enum {
    XR_OK = 0,

    // Lexer errors (1-99)
    XR_ERR_LEXER_INVALID_CHAR = 1,
    XR_ERR_LEXER_UNTERMINATED_STRING,
    XR_ERR_LEXER_INVALID_NUMBER,

    // Syntax errors (100-199)
    XR_ERR_SYNTAX = 100,
    XR_ERR_SYNTAX_UNEXPECTED_TOKEN,
    XR_ERR_SYNTAX_EXPECT_EXPRESSION,
    XR_ERR_SYNTAX_EXPECT_SEMICOLON,
    XR_ERR_SYNTAX_EXPECT_RPAREN,
    XR_ERR_SYNTAX_EXPECT_RBRACE,
    XR_ERR_SYNTAX_EXPECT_RBRACKET,
    XR_ERR_SYNTAX_INVALID_ASSIGNMENT,

    // Compile errors (200-299)
    XR_ERR_COMPILE = 200,
    XR_ERR_COMPILE_TOO_MANY_LOCALS,
    XR_ERR_COMPILE_TOO_MANY_CONSTANTS,
    XR_ERR_COMPILE_TOO_MANY_UPVALUES,
    XR_ERR_COMPILE_VARIABLE_REDEFINED,
    XR_ERR_COMPILE_UNDEFINED_VARIABLE,
    XR_ERR_COMPILE_JUMP_TOO_LARGE,

    // Type errors (300-349)
    XR_ERR_TYPE = 300,
    XR_ERR_TYPE_NOT_CALLABLE = 304,
    XR_ERR_TYPE_NOT_INDEXABLE,
    XR_ERR_TYPE_NOT_ITERABLE,
    XR_ERR_TYPE_INVALID_OPERAND,

    // Static analysis errors (350-399)
    XR_ERR_ANALYZE = 350,
    XR_ERR_ANALYZE_UNDEFINED_VAR,       // Undeclared variable
    XR_ERR_ANALYZE_TYPE_MISMATCH,       // Type not assignable
    XR_ERR_ANALYZE_CONST_ASSIGN,        // Cannot assign to const
    XR_ERR_ANALYZE_NOT_CALLABLE,        // Value is not callable
    XR_ERR_ANALYZE_WRONG_ARG_COUNT,     // Wrong argument count
    XR_ERR_ANALYZE_ARG_TYPE,            // Argument type mismatch
    XR_ERR_ANALYZE_GENERIC_COUNT,       // Wrong type argument count
    XR_ERR_ANALYZE_GENERIC_CONSTRAINT,  // Type constraint not satisfied
    XR_ERR_ANALYZE_SUPER_FIRST,         // super() must be first statement
    XR_ERR_ANALYZE_SUPER_THIS,          // Cannot use 'this' before super()
    XR_ERR_ANALYZE_SUPER_REQUIRED,      // Must call super() in derived class
    XR_ERR_ANALYZE_SUPER_INVALID,       // super() in non-derived class
    XR_ERR_ANALYZE_CLOSURE_CAPTURE,     // Unsafe closure capture in coroutine
    XR_ERR_ANALYZE_AWAIT_TYPE,          // await expects Task type
    XR_ERR_ANALYZE_MISSING_TYPE,        // Variable needs type annotation or initializer

    // Runtime errors (400-499) — most runtime codes in xerror_codes.h
    XR_ERR_RUNTIME = 400,

    // Memory errors (500-599)
    XR_ERR_MEMORY = 500,
    XR_ERR_MEMORY_ALLOCATION_FAILED,
    XR_ERR_MEMORY_OUT_OF_MEMORY,

    // IO errors (600-699)
    XR_ERR_IO = 600,
    XR_ERR_IO_FILE_NOT_FOUND,
    XR_ERR_IO_READ_FAILED,
    XR_ERR_IO_WRITE_FAILED,
    XR_ERR_IO_PERMISSION_DENIED,

    // Module errors (700-749)
    XR_ERR_MODULE = 700,
    XR_ERR_MODULE_NOT_FOUND,
    XR_ERR_MODULE_LOAD_FAILED,
    XR_ERR_MODULE_CIRCULAR_IMPORT,
    XR_ERR_MODULE_EXPORT_NOT_FOUND,

    // Coroutine errors (750-799)
    XR_ERR_COROUTINE = 750,
    XR_ERR_COROUTINE_DEADLOCK,
    XR_ERR_COROUTINE_CANCELLED,
    XR_ERR_COROUTINE_CHANNEL_CLOSED,
    XR_ERR_COROUTINE_LIMIT_EXCEEDED,

    // JSON errors (800-849)
    XR_ERR_JSON = 800,
    XR_ERR_JSON_PARSE_FAILED,
    XR_ERR_JSON_INVALID_VALUE,

    // Regex errors (850-899)
    XR_ERR_REGEX = 850,
    XR_ERR_REGEX_COMPILE_FAILED,
    XR_ERR_REGEX_INVALID_PATTERN,

    // Other errors
    XR_ERR_INTERNAL = 900,
    XR_ERR_NOT_IMPLEMENTED = 901,
    XR_ERR_UNKNOWN = 999
} XrErrorCode;

/* ========== Forward Declarations ========== */

#include "../base/xforward_decl.h"
typedef struct XrError XrError;

#endif  // XERROR_H
