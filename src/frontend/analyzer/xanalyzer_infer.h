/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_infer.h - Type inference for expressions and statements
 *
 * KEY CONCEPT:
 *   Multi-pass analysis:
 *   1. Symbol collection - gather all declarations
 *   2. Type inference - compute types bottom-up
 *   3. Type checking - verify type compatibility
 */

#ifndef XANALYZER_INFER_H
#define XANALYZER_INFER_H

#include "xanalyzer.h"
#include "xanalyzer_flow.h"
#include "../../base/xdefs.h"

typedef struct XaLoopScope {
    const char *label;
    int line;
    struct XaScope *entry_scope;
    struct XaLoopScope *prev;
} XaLoopScope;

typedef struct XaActiveSliceBorrow {
    XaLoan loan;
    struct XaSymbol *owner_symbol;
    char *owner_path;
    struct XaSymbol *view_symbol;
    struct XaScope *view_scope;
    int loop_depth_at_creation;
    bool is_pointer_borrow;
    struct XaActiveSliceBorrow *next;
} XaActiveSliceBorrow;

typedef struct XaInferVar {
    uint32_t id;
    const char *reason;
    XrLocation loc;
    XrType **lower_bounds;
    int lower_bound_count;
    int lower_bound_capacity;
    XrType **upper_bounds;
    int upper_bound_count;
    int upper_bound_capacity;
    XrType **constraints;
    int constraint_count;
    int constraint_capacity;
    XrType *solution;
    bool reported_unsolved;
    struct XaInferVar *next;
} XaInferVar;

#define XA_BLOCK_CURSOR_MAX 64

// Inference context (for a single file/function)
typedef struct XaInferContext {
    XaAnalyzer *analyzer;
    XaFlowBuilder *flow;
    XaFlowCache *cache;

    // Current function being analyzed
    XaSymbol *current_function;
    XrType *expected_return_type;
    // Generator detection: set true when a `yield expr` is seen in the current
    // function body; the function-decl handler then marks it as a generator.
    bool current_fn_has_yield;

    // Current class context (set while inferring a class/struct method body).
    // Used to enforce private/protected member visibility and const-field writes.
    struct XrClassInfo *current_class_info;
    const char *current_class_name;
    bool current_method_is_constructor;

    // Collected return types (for inference)
    XrType **return_types;
    int return_type_count;
    int return_type_capacity;
    uint8_t return_storage_domain;
    bool return_storage_known;
    bool return_storage_mixed;
    bool return_storage_unknown;

    // File info
    const char *file_path;

    // Expected type for bidirectional inference (contextual typing)
    // Propagated from declaration to initializer expression
    XrType *expected_type;

    // Analyzer-owned inference variables. These are not XrTypeKind values:
    // unresolved variables must be diagnosed and converted to ErrorType recovery
    // before any semantic XrType is exposed to later phases.
    uint32_t next_infer_var_id;
    XaInferVar *infer_vars;
    int infer_var_count;
    int unresolved_infer_var_count;

    // `copy(view-producing-expr)` is the one context where a slice may first be
    // typed as a Slice without an explicit Slice target; the copy result is owned.
    bool allow_view_expr_for_copy;

    // Payload enum variants are constructors, not first-class values. The call
    // visitor sets this while typing the callee of `Enum.Variant(...)`.
    bool allow_payload_enum_ctor_value;

    // The variable being declared is not visible inside its own initializer.
    // This lets `var copy = copy(x)` call the outer/builtin `copy` while still
    // reporting `var x = x` as an unresolved self-reference when no outer x exists.
    uint32_t initializing_symbol_id;

    // Active only while inferring the source operand of `move x`. Repeated
    // inference of the same AST can leave a moved mark on x from the previous
    // visit; the current move operand is still the operation that performs the
    // transfer, not a post-move use.
    const AstNode *current_move_source_node;
    uint32_t current_move_source_symbol_id;
    bool current_move_source_allows_stale_mark;

    // Generic type inference context (for callback parameters)
    // e.g., arr.map(x => x+1) - element_type is int, so x: int
    XrType *callback_element_type;      // Element type for callback first param
    XrType *callback_index_type;        // Index type for callback second param (always int)
    XrType *callback_accumulator_type;  // Accumulator type for reduce (from initial value)
    XrType *callback_array_type;        // Original array type for callbacks needing it

    // Nonzero inside an `unsafe { }` region. Gates raw-pointer dereference and
    // extern-function calls: those are errors at depth 0 (Rust model).
    int unsafe_depth;

    // Nonzero while analyzing a `comptime {}` statement block. Local bindings
    // in this scope must carry compile-time values and are erased before lowering.
    int comptime_block_depth;

    // Active loop stack for validating break/continue and resolving labels.
    XaLoopScope *loop_scope;
    int loop_depth;

    // Active while inferring a stdlib `parallel.*` callback. Captures from
    // outside this scope are restricted so hosted VM/AOT lowering never
    // inherits a data race. pending_parallel_callback_name is set by call
    // analysis before visiting the lambda; xa_visit_function_expr converts it
    // into the concrete function scope after parameters are registered.
    const char *pending_parallel_callback_name;
    bool in_parallel_callback_body;
    XaScope *parallel_callback_scope;
    const char *parallel_callback_name;

    // Active while checking a sys.Thread.spawn body. That body runs in the OS
    // thread domain, so ThreadLocal usage is intentional there.
    int os_thread_body_depth;

    // Active local Slice/Slice<byte> views keyed by the owning mutable container.
    // Used to reject owner grow/free mutations while borrowed views are live.
    XaActiveSliceBorrow *active_span_borrows;

    // Current statement cursor inside xa_visit_block_stmt. Used by Slice borrow
    // liveness to allow owner mutations after the borrowed view's last use in a
    // straight-line block.
    AstNode *current_block_node;
    int current_block_stmt_index;

    // Stack of active statement cursors. This lets Slice liveness look past an
    // inner branch/block to the continuation of its parent blocks.
    AstNode *block_cursor_nodes[XA_BLOCK_CURSOR_MAX];
    int block_cursor_indices[XA_BLOCK_CURSOR_MAX];
    int block_cursor_depth;
} XaInferContext;

// API: Context lifecycle
XR_FUNC XaInferContext *xa_infer_context_new(XaAnalyzer *analyzer);
XR_FUNC void xa_infer_context_free(XaInferContext *ctx);

// Inference variables
XR_FUNC XaInferVar *xa_infer_var_new(XaInferContext *ctx, const char *reason,
                                     const XrLocation *loc);
XR_FUNC XrType *xa_infer_var_report_unsolved(XaInferContext *ctx, XaInferVar *var,
                                             const char *message);

// Return type inference
XR_FUNC void xa_infer_add_return_type(XaInferContext *ctx, XrType *type);
XR_FUNC XrType *xa_infer_compute_return_type(XaInferContext *ctx);

#endif  // XANALYZER_INFER_H
