/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrepl.h - Symbol metadata still consumed by Xi lowering
 *
 * KEY CONCEPT:
 *   The remaining Xi lowerer consumes these borrowed symbol facts and storage
 *   keys. They are not a source compilation, execution or inspection interface.
 */

#ifndef XREPL_H
#define XREPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../base/xdefs.h"

typedef struct XrString XrString;
typedef struct XrType XrType;

/* ========== REPL Symbol Table ========== */

typedef struct XrReplSymbol {
    XrString *name;
    /* Borrowed compiler type; the table does not retain the analyzer. */
    XrType *type;
    uint32_t symbol_id;
    bool is_const;
} XrReplSymbol;

typedef struct XrReplSymbolTable {
    XrReplSymbol *symbols;
    int count;
    int capacity;

    /* Versioned result bindings remain separate from declaration slots. */
    XrReplSymbol *results;
    int result_count;
    int result_capacity;
    XrString *latest_result_name;
    uint64_t next_result_id;
} XrReplSymbolTable;

XR_FUNC bool xr_repl_binding_key(uint32_t symbol_id, char *buffer, size_t capacity);

#endif // XREPL_H
