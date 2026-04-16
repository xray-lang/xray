/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrepl.h - REPL incremental execution support
 *
 * KEY CONCEPT:
 *   Persistent symbol table that survives across REPL compilation units.
 *   Maps variable names to absolute shared array indices, enabling
 *   incremental compilation without replaying history code.
 *
 * WHY THIS DESIGN:
 *   - Each REPL input compiles as independent unit (fresh XrCompilerContext)
 *   - Without this, new compilations cannot resolve names from prior inputs
 *   - Symbol table seeds each new compiler context with prior definitions
 *   - shared_offset=0 in REPL mode: all indices are absolute
 */

#ifndef XREPL_H
#define XREPL_H

#include <stdbool.h>
#include "../base/xdefs.h"

// Forward declarations
typedef struct XrayIsolate XrayIsolate;
typedef struct XrCompilerContext XrCompilerContext;
typedef struct XrString XrString;
typedef struct XrProto XrProto;

/* ========== REPL Symbol Table ========== */

typedef struct XrReplSymbol {
    XrString *name;
    int shared_index;       // absolute index in isolate->vm.shared
    bool is_const;
} XrReplSymbol;

typedef struct XrReplSymbolTable {
    XrReplSymbol *symbols;
    int count;
    int capacity;
} XrReplSymbolTable;

// Lifecycle
XR_FUNC XrReplSymbolTable* xr_repl_symbols_new(void);
XR_FUNC void xr_repl_symbols_free(XrReplSymbolTable *table);
XR_FUNC void xr_repl_symbols_clear(XrReplSymbolTable *table);

// Seed compiler context with prior definitions
XR_FUNC void xr_repl_symbols_seed_context(XrReplSymbolTable *table, XrCompilerContext *ctx);

// Collect new definitions from compiler context after compilation
XR_FUNC void xr_repl_symbols_collect(XrReplSymbolTable *table, XrCompilerContext *ctx,
                             int seeded_count);

/* ========== REPL Input Completeness Check ========== */

typedef enum {
    XR_INPUT_COMPLETE,      // Structurally complete, ready to compile
    XR_INPUT_INCOMPLETE,    // Unclosed brackets, strings, or comments
} XrInputStatus;

/*
 * Check if REPL input is structurally complete using the lexer.
 * Scans all tokens and tracks bracket depth and unterminated literals.
 * Replaces manual bracket tracking (update_depth) with compiler-accurate detection.
 */
XR_FUNC XrInputStatus xr_repl_check_input(const char *source);

/* ========== REPL Compilation & Execution ========== */

/*
 * Compile source for REPL incremental execution.
 * - Seeds compiler context from isolate->repl_symbols
 * - Uses shared_offset=0 (absolute indices)
 * - Updates repl_symbols with new definitions
 * Returns compiled proto, or NULL on error.
 */
XR_FUNC XrProto* xr_repl_compile(XrayIsolate *isolate, const char *source);

#endif // XREPL_H
