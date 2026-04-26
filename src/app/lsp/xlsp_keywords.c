/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_keywords.c - Keyword and builtin array definitions
 */

#include "xlsp_keywords.h"
#include <stddef.h>

// xray language keywords
const char *xr_keywords[] = {
    // Declarations
    "let", "const", "fn", "class", "interface", "enum", "type",
    // Control flow
    "if", "else", "while", "for", "in", "is", "to", "break", "continue", "return", "match",
    "default",
    // Class
    "extends", "implements", "constructor", "this", "super", "new", "static", "private", "public",
    "abstract", "override", "operator",
    // Exception
    "try", "catch", "finally", "throw",
    // Module
    "import", "export", "from", "as",
    // Coroutine
    "go", "await", "select", "defer", "scope", "cancelled", "shared", "after",
    // Literals
    "true", "false", "null",
    // Types
    "void", "int", "float", "string", "bool", "Array", "Map", "Set", "Json", "Channel", "Bytes",
    "BigInt", "StringBuilder", "Exception", "Regex", NULL};

// Builtin functions
const char *xr_builtins[] = {"print",       "dump",         "typeof",    "typename",  "assert",
                             "assert_true", "assert_false", "assert_eq", "assert_ne", "int",
                             "float",       "string",       "bool",      "copy",      "chr",
                             "Coro",        "CoroPool",     "Reflect",   "Type",      NULL};
