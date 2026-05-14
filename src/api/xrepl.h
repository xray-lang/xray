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
 *   Records which names exist and whether they are const, so each new
 *   compiler context can resolve names from prior inputs.
 *   Runtime values live in the globals dict (OP_GETGLOBAL/OP_SETGLOBAL);
 *   the symbol table is metadata only.
 */

#ifndef XREPL_H
#define XREPL_H

#include <stdbool.h>
#include <stdint.h>
#include "../base/xdefs.h"

// Forward declarations
typedef struct XrayIsolate XrayIsolate;
typedef struct XrCompilerContext XrCompilerContext;
typedef struct XrString XrString;
typedef struct XrProto XrProto;

/* ========== REPL Symbol Table ========== */

typedef struct XrReplSymbol {
    XrString *name;
    bool is_const;
} XrReplSymbol;

typedef struct XrReplSymbolTable {
    XrReplSymbol *symbols;
    int count;
    int capacity;
} XrReplSymbolTable;

// Lifecycle
XR_FUNC XrReplSymbolTable *xr_repl_symbols_new(void);
XR_FUNC void xr_repl_symbols_free(XrReplSymbolTable *table);
XR_FUNC void xr_repl_symbols_clear(XrReplSymbolTable *table);

/* Return the isolate's REPL symbol table (NULL before any REPL compile).
 * Read-only view for tab completion / introspection by CLI / embedders;
 * callers must not free the table. */
XR_FUNC XrReplSymbolTable *xr_repl_symbols_of(XrayIsolate *isolate);

/* Return the C string for a REPL symbol's name without leaking the
 * XrString definition to callers (xrepl.h forward-declares XrString). */
XR_FUNC const char *xr_repl_symbol_cname(const XrReplSymbol *sym);

/* Look up a REPL binding by name and, if it currently holds an integer
 * value, copy it to *out and return true.  Returns false if the
 * binding does not exist or holds a non-integer value.  Intended for
 * tests and tools that need to verify scalar binding state without
 * pulling in the full XrValue ABI. */
XR_FUNC bool xr_repl_peek_int(XrayIsolate *isolate, const char *name, int64_t *out);

// Seed compiler context with prior definitions
XR_FUNC void xr_repl_symbols_seed_context(XrReplSymbolTable *table, XrCompilerContext *ctx);

/* ========== REPL Input Completeness Check ========== */

typedef enum {
    XR_INPUT_COMPLETE,    // Structurally complete, ready to compile
    XR_INPUT_INCOMPLETE,  // Unclosed brackets, strings, or comments
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
 * - Seeds compiler context from isolate->repl_symbols (name metadata)
 * - Emits OP_GETGLOBAL/OP_SETGLOBAL for top-level variable access
 * - Updates repl_symbols with new definitions
 * Returns compiled proto, or NULL on error.
 */
XR_FUNC XrProto *xr_repl_compile(XrayIsolate *isolate, const char *source);

/* ========== Interactive Inspection ========== */

/*
 * Pretty-print every top-level binding currently visible to the REPL.
 * One line per symbol: "name : typeName = formatted value".  Reads
 * names from isolate->repl_symbols, values from the globals dict.
 * Cheap, no compilation or execution side effects.  Safe to call
 * before the first compile (prints nothing).
 */
XR_FUNC void xr_repl_print_vars(XrayIsolate *isolate);

/*
 * Show the runtime type name of `expr`.  Synthesises and runs
 * `print(typename(<expr>))` through the normal incremental compile
 * pipeline so the expression sees the same scope as bare user input.
 *
 * `expr` is evaluated; for a side-effect-free static-only variant,
 * use the analyzer directly.  Empty / NULL expr is a user error and
 * reports a message without aborting.
 */
XR_FUNC void xr_repl_print_type(XrayIsolate *isolate, const char *expr);

#endif  // XREPL_H
