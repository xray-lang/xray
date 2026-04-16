/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobals_table.h - Dynamic global variable table
 *
 * KEY CONCEPT:
 *   Index-based global variable storage for O(1) access.
 *   Compiler assigns each global a unique index at compile time.
 *
 * WHY NOT HASH TABLE:
 *   - Index lookup is faster than string hash lookup
 *   - Compiler already resolves names to indices
 *   - Simpler GC root scanning (just iterate array)
 *
 * THREAD SAFETY:
 *   Currently NOT thread-safe. Each Isolate has its own table.
 *   For multi-isolate scenarios, each isolate is independent.
 *
 * RELATED MODULES:
 *   - xcompiler.h: Assigns global indices during compilation
 *   - xvm.c: Uses OP_GETBUILTIN/OP_SETGLOBAL with indices
 */

#ifndef XGLOBALS_TABLE_H
#define XGLOBALS_TABLE_H

#include "value/xvalue.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct XrGlobalsTable {
    XrValue *values;
    size_t capacity;
    size_t count;
} XrGlobalsTable;

XR_FUNC XrGlobalsTable* xr_globals_create(size_t initial_capacity);
XR_FUNC void xr_globals_destroy(XrGlobalsTable *globals);
XR_FUNC int xr_globals_add(XrGlobalsTable *globals, XrValue value);
XR_FUNC XrValue xr_globals_get(XrGlobalsTable *globals, int index);
XR_FUNC bool xr_globals_set(XrGlobalsTable *globals, int index, XrValue value);
XR_FUNC size_t xr_globals_count(XrGlobalsTable *globals);
XR_FUNC bool xr_globals_resize(XrGlobalsTable *globals, size_t new_capacity);

#endif // XGLOBALS_TABLE_H
