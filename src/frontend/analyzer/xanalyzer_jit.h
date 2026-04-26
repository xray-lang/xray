/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_jit.h - JIT/AOT metadata generation from analyzer
 *
 * KEY CONCEPT:
 *   Pass 3 runs after type inference (Pass 2) to generate metadata
 *   that JIT/AOT compilers need for optimization decisions.
 *   Controlled by XR_ENABLE_JIT / XR_ENABLE_AOT compile flags.
 *
 * WHY THIS DESIGN:
 *   - Separated from core analysis to keep interpreter builds lean
 *   - Uses existing compile_type and symbol info, no re-analysis needed
 *   - Metadata lifetime matches compilation unit (arena-friendly)
 */

#ifndef XANALYZER_JIT_H
#define XANALYZER_JIT_H

#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../../runtime/value/xtype.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

/* ========== Function-Level Type Summary ========== */

// Function property flags (bitfield)
typedef enum XaFuncFlags {
    XA_FUNC_NONE = 0,
    XA_FUNC_PURE = (1 << 0),           // No side effects, same input = same output
    XA_FUNC_NO_THROW = (1 << 1),       // Never throws exceptions
    XA_FUNC_LEAF = (1 << 2),           // Calls no other user functions
    XA_FUNC_RECURSIVE = (1 << 3),      // Directly or indirectly recursive
    XA_FUNC_VARIADIC = (1 << 4),       // Has rest parameter
    XA_FUNC_GENERATOR = (1 << 5),      // Contains yield (future)
    XA_FUNC_COROUTINE = (1 << 6),      // Contains go/await
    XA_FUNC_CLOSURE = (1 << 7),        // Captures upvalues
    XA_FUNC_SMALL = (1 << 8),          // Body < N statements, inline candidate
    XA_FUNC_HOT_CANDIDATE = (1 << 9),  // Called in loop or called frequently
} XaFuncFlags;

// Per-function type summary for JIT/AOT
typedef struct XaFuncSummary {
    uint32_t symbol_id;     // Symbol ID of the function
    const char *name;       // Function name (for debug)
    XrType *signature;      // Complete function type
    XrType *actual_return;  // Inferred return type (may be more precise than declared)
    uint32_t flags;         // XaFuncFlags bitfield
    uint32_t body_hash;     // Content hash for incremental JIT invalidation
    int call_count;         // Static call count (from reference tracking)
    int loop_depth;         // Max loop nesting depth in body
    int stmt_count;         // Statement count (for inline heuristic)
    int local_count;        // Number of local variables
} XaFuncSummary;

/* ========== Per-Variable JIT Metadata ========== */

// Compact per-variable info for JIT register allocation
typedef struct XaVarHint {
    uint32_t symbol_id;
    uint8_t opt_hint;   // XrOptHint value
    uint8_t certainty;  // XaJitCertainty
    uint8_t stability;  // XaTypeStability
    uint8_t flags;      // nullable(1), const(2), loop_var(4), const_foldable(8)
} XaVarHint;

#define XA_VAR_FLAG_NULLABLE (1 << 0)
#define XA_VAR_FLAG_CONST (1 << 1)
#define XA_VAR_FLAG_LOOP_VAR (1 << 2)
#define XA_VAR_FLAG_CONST_FOLDABLE (1 << 3)
#define XA_VAR_FLAG_SHARED (1 << 4)

/* ========== JIT Metadata Collection ========== */

// Collected metadata for a single file/module
typedef struct XaJitMetadata {
    XaFuncSummary *func_summaries;  // Array of function summaries
    int func_count;
    int func_capacity;

    XaVarHint *var_hints;  // Array of variable hints
    int var_count;
    int var_capacity;
} XaJitMetadata;

// API: Create/free JIT metadata
XR_FUNC XaJitMetadata *xa_jit_metadata_new(void);
XR_FUNC void xa_jit_metadata_free(XaJitMetadata *meta);

// API: Pass 3 entry point - generate JIT metadata after type inference
// This walks the symbol table and AST to collect optimization metadata.
// Only called when JIT/AOT is enabled (controlled by caller).
XR_FUNC void xa_generate_jit_metadata(XaAnalyzer *analyzer, void *ast, XaJitMetadata *out);

// API: Query function summary by symbol ID
XR_FUNC XaFuncSummary *xa_jit_get_func_summary(XaJitMetadata *meta, uint32_t symbol_id);

// API: Query variable hint by symbol ID
XR_FUNC XaVarHint *xa_jit_get_var_hint(XaJitMetadata *meta, uint32_t symbol_id);

#endif  // XANALYZER_JIT_H
