/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_stdlib.h - Built-in standard library symbols for LSP
 *
 * KEY CONCEPT:
 *   Pre-defined symbols for standard library modules.
 *   Provides completion, hover, and signature help without parsing stdlib source.
 */

#ifndef XLSP_STDLIB_H
#define XLSP_STDLIB_H

#include <stdbool.h>
#include "../../base/xdefs.h"

// Symbol kind (matches LSP SymbolKind)
typedef enum {
    XLSP_SYM_FUNCTION = 12,
    XLSP_SYM_VARIABLE = 13,
    XLSP_SYM_CONSTANT = 14,
    XLSP_SYM_CLASS = 5,
    XLSP_SYM_METHOD = 6,
    XLSP_SYM_PROPERTY = 7,
} XlspSymbolKind;

// Function parameter info
typedef struct {
    const char *name;
    const char *type;
    const char *doc;
} XlspParamInfo;

// Symbol info
typedef struct {
    const char *name;
    XlspSymbolKind kind;
    const char *signature;      // e.g. "fn(ms: int): void"
    const char *documentation;
    const XlspParamInfo *params;
    int param_count;
} XlspSymbolInfo;

// Module info
typedef struct {
    const char *name;
    const char *documentation;
    const XlspSymbolInfo *symbols;
    int symbol_count;
} XlspModuleInfo;

// Get all standard library modules
XR_FUNC const XlspModuleInfo *xlsp_stdlib_get_modules(int *count);

// Find a module by name
XR_FUNC const XlspModuleInfo *xlsp_stdlib_find_module(const char *name);

// Find a symbol in a module
XR_FUNC const XlspSymbolInfo *xlsp_stdlib_find_symbol(const XlspModuleInfo *module, 
                                               const char *name);

#endif // XLSP_STDLIB_H
