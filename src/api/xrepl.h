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
 *   Records each published analyzer symbol identity, exact inferred type, and
 *   constness so each new compiler context consumes typed prior-input facts.
 *   Runtime values live in the globals dict (OP_GETGLOBAL/OP_SETGLOBAL);
 *   the symbol table is metadata only.
 */

#ifndef XREPL_H
#define XREPL_H

#include <stdbool.h>
#include <stdint.h>
#include "../base/xdefs.h"

// Forward declarations
typedef struct XrVMRuntime XrVMRuntime;
typedef struct XrCompilerContext XrCompilerContext;
typedef struct XrCompilerSession XrCompilerSession;
typedef struct XrModuleIdentityAuthority XrModuleIdentityAuthority;
typedef struct XrString XrString;
typedef struct XrProto XrProto;
typedef struct XrType XrType;

/* ========== REPL Symbol Table ========== */

typedef struct XrReplSymbol {
    XrString *name;
    /* Borrowed from the session-owned persistent analyzer. */
    XrType *type;
    uint32_t symbol_id;
    bool is_const;
} XrReplSymbol;

typedef struct XrReplSymbolTable {
    XrReplSymbol *symbols;
    int count;
    int capacity;

    /* Versioned implicit results are compiler/runtime storage, not user
     * declarations. Keep them separate so completion and `.vars` expose only
     * the stable `it` alias instead of implementation names. */
    XrReplSymbol *results;
    int result_count;
    int result_capacity;
    XrString *latest_result_name;
    uint64_t next_result_id;
} XrReplSymbolTable;

// Lifecycle
XR_FUNC XrReplSymbolTable *xr_repl_symbols_new(void);
XR_FUNC void xr_repl_symbols_free(XrReplSymbolTable *table);

/* Return the isolate's REPL symbol table (NULL before any REPL compile).
 * Read-only view for tab completion / introspection by CLI / embedders;
 * callers must not free the table. */
XR_FUNC XrReplSymbolTable *xr_repl_symbols_of(XrVMRuntime *isolate);

/* Return the C string for a REPL symbol's name without leaking the
 * XrString definition to callers (xrepl.h forward-declares XrString). */
XR_FUNC const char *xr_repl_symbol_cname(const XrReplSymbol *sym);

/* Look up a REPL binding by name and, if it currently holds an integer
 * value, copy it to *out and return true.  Returns false if the
 * binding does not exist or holds a non-integer value.  Intended for
 * tests and tools that need to verify scalar binding state without
 * pulling in the full XrValue ABI. */
XR_FUNC bool xr_repl_peek_int(XrVMRuntime *isolate, const char *name, int64_t *out);

/* Whether this session has a successfully evaluated, meaningful last result. */
XR_FUNC bool xr_repl_has_last_result(XrVMRuntime *isolate);

// Seed compiler context with typed prior definitions
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

/* ========== REPL Evaluation ========== */

typedef enum XrReplEvalStatus {
    XR_REPL_EVAL_OK,
    XR_REPL_EVAL_COMPILE_ERROR,
    XR_REPL_EVAL_RUNTIME_ERROR,
} XrReplEvalStatus;

typedef struct XrReplEvalResult {
    /* Non-NULL whenever compilation succeeded. The caller owns the proto and
     * must retain it while closures produced by this submission can survive. */
    XrProto *proto;
    XrReplEvalStatus status;
} XrReplEvalResult;

/*
 * Compile and execute one REPL submission atomically with respect to the
 * implicit last-result binding.
 * - Seeds compiler context from session-owned repl_symbols (name metadata)
 * - Emits OP_GETGLOBAL/OP_SETGLOBAL for top-level variable access
 * - Publishes declarations and a new `it` only after successful execution
 * - Returns the compiled proto even on runtime failure so callers can free it
 */
XR_FUNC XrReplEvalResult xr_repl_eval(XrCompilerSession *session, XrVMRuntime *vm_host,
                                      const char *source,
                                      const XrModuleIdentityAuthority *authority);

/* ========== Interactive Inspection ========== */

/*
 * Pretty-print every top-level binding currently visible to the REPL.
 * One line per symbol: "name : typeName = formatted value".  Reads
 * names from session-owned repl_symbols, values from the globals dict.
 * Cheap, no compilation or execution side effects.  Safe to call
 * before the first compile (prints nothing).
 */
XR_FUNC void xr_repl_print_vars(XrVMRuntime *isolate);

/*
 * Show the runtime type name of `expr`.  Synthesises and runs
 * `print(typeName(<expr>))` through the normal incremental compile
 * pipeline so the expression sees the same scope as bare user input.
 *
 * `expr` is evaluated; for a side-effect-free static-only variant,
 * use the analyzer directly.  Empty / NULL expr is a user error and
 * reports a message without aborting.
 */
XR_FUNC void xr_repl_print_type(XrVMRuntime *isolate, const char *expr,
                                const XrModuleIdentityAuthority *authority);

#endif  // XREPL_H
