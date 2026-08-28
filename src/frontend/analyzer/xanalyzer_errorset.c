/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_errorset.c - Error set inference pass (Pass 3)
 *
 * After Pass 2 (type inference) has resolved all types, this pass
 * walks the AST to infer which enum error types each function may
 * throw.  The result is interned in XaAnalyzer.effect_db and stored
 * as XaSymbolLinks.effect_id.
 *
 * Algorithm:
 *   1. Collect all function symbols in topological order.
 *   2. For each function, walk its body:
 *      - `throw E.Case` => add E to error set
 *      - call to fallible function => union callee's error set
 *      - `catch (e) {}` => subtract caught errors
 *   3. Fixpoint iteration for recursive/mutually-recursive functions.
 */

#include "xanalyzer_errorset.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_visitor.h"
#include "../parser/xtype_ref.h"
#include "xa_effect_db.h"
#include "xa_selection.h"
#include "xbuiltin_receiver_registry.h"
#include "xtype_ref_resolve.h"
#include "../../runtime/class/xclass_info.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xmalloc.h"
#include <string.h>

static void es_summary_add_enum_all(XaEffectDatabase *db, XaEffectSummary *summary,
                                    XrType *enum_type) {
    if (!db || !summary || !enum_type)
        return;
    XaErrorTypeId type_id = xa_effect_db_register_error_enum(db, enum_type);
    if (type_id == XA_ERROR_TYPE_NONE)
        return;
    xa_effect_summary_add_all_variants(db, summary, type_id);
}

static void es_summary_add_enum_case(XaEffectDatabase *db, XaEffectSummary *summary,
                                     XrType *enum_type, uint32_t case_index) {
    if (!db || !summary || !enum_type)
        return;
    XaErrorTypeId type_id = xa_effect_db_register_error_enum(db, enum_type);
    if (type_id == XA_ERROR_TYPE_NONE)
        return;
    XaErrorVariantId variant_id = case_index;
    const XrEnumLayout *layout = enum_type->enum_type.layout;
    if (layout && case_index >= layout->variant_count)
        return;
    xa_effect_summary_add_variant(db, summary, type_id, variant_id);
}

typedef struct ErrorSetCtx ErrorSetCtx;

typedef struct CatchEffectPattern {
    bool catch_all;
    bool has_enum;
    bool has_variant;
    XrType *enum_type;
    XaErrorTypeId type_id;
    XaErrorVariantId variant_id;
} CatchEffectPattern;

static const char *catch_pattern_variant_name(AstNode *pattern) {
    if (!pattern)
        return NULL;
    if (pattern->type == AST_PATTERN_ADT) {
        AstNode *variant = pattern->as.pattern_adt.variant;
        if (!variant)
            return NULL;
        if (variant->type == AST_ENUM_ACCESS)
            return variant->as.enum_access.member_name;
        if (variant->type == AST_MEMBER_ACCESS)
            return variant->as.member_access.name;
        return NULL;
    }
    if (pattern->type != AST_PATTERN_LITERAL || !pattern->as.pattern_literal.value)
        return NULL;
    AstNode *value = pattern->as.pattern_literal.value;
    if (value->type == AST_ENUM_ACCESS)
        return value->as.enum_access.member_name;
    if (value->type == AST_MEMBER_ACCESS)
        return value->as.member_access.name;
    return NULL;
}

static bool catch_pattern_is_wildcard(AstNode *pattern) {
    return pattern && pattern->type == AST_PATTERN_WILDCARD;
}

static int enum_variant_index_for_name(XaAnalyzer *analyzer, XrType *enum_type,
                                       const char *variant_name) {
    if (!analyzer || !enum_type || !variant_name || !XR_TYPE_IS_ENUM(enum_type))
        return -1;
    const XrEnumLayout *layout = enum_type->enum_type.layout;
    if (layout) {
        for (uint32_t i = 0; i < layout->variant_count; i++) {
            if (layout->variants[i].name && strcmp(layout->variants[i].name, variant_name) == 0)
                return (int) i;
        }
    }
    const char *enum_name = enum_type->enum_type.enum_name;
    if (!enum_name)
        return -1;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, enum_name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_in_scope(analyzer, enum_name, analyzer->global_scope);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_deep(analyzer, enum_name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
    return links && links->enum_info ? xa_enum_info_find_variant(links->enum_info, variant_name)
                                     : -1;
}

static CatchEffectPattern catch_effect_pattern(XaAnalyzer *analyzer, XrCatchClause *cc) {
    CatchEffectPattern result = {0};
    if (!analyzer || !cc || cc->is_panic)
        return result;
    if (!cc->type) {
        if (!cc->pattern || catch_pattern_is_wildcard(cc->pattern))
            result.catch_all = true;
        return result;
    }
    XrType *catch_type = xr_tref_resolve_in_analyzer(analyzer, cc->type);
    if (!catch_type || !XR_TYPE_IS_ENUM(catch_type))
        return result;
    result.enum_type = catch_type;
    result.type_id = xa_effect_db_register_error_enum(analyzer->effect_db, catch_type);
    result.has_enum = result.type_id != XA_ERROR_TYPE_NONE;
    const char *variant_name = catch_pattern_variant_name(cc->pattern);
    if (variant_name) {
        int index = enum_variant_index_for_name(analyzer, catch_type, variant_name);
        if (index >= 0) {
            result.has_variant = true;
            result.variant_id = (XaErrorVariantId) index;
        }
    }
    return result;
}

/* ========== Internal Context ========== */

typedef struct FunctionValueTarget {
    XaSymbol *symbol;
    AstNode *function_expr;
    XaSymbol *target_symbols[8];
    AstNode *target_function_exprs[8];
    int target_count;
} FunctionValueTarget;

typedef struct FunctionValueAliasState {
    uint32_t ids[128];
    FunctionValueTarget targets[128];
    int count;
} FunctionValueAliasState;

typedef enum CatchAggregateAliasKind {
    CATCH_AGGREGATE_EMPTY,
    CATCH_AGGREGATE_SINGLETON,
    CATCH_AGGREGATE_KNOWN_COUNT,
    CATCH_AGGREGATE_INDEX,
    CATCH_AGGREGATE_KEY_INDEX,
    CATCH_AGGREGATE_PRESENT_INDEX,
    CATCH_AGGREGATE_FIELD,
    CATCH_AGGREGATE_ELEMENT,
    CATCH_AGGREGATE_KEY_ELEMENT,
    CATCH_AGGREGATE_ENTRY_KEY,
    CATCH_AGGREGATE_ENTRY_VALUE,
} CatchAggregateAliasKind;

typedef struct CatchAggregateAlias {
    uint32_t container_id;
    const char *container_name;
    CatchAggregateAliasKind kind;
    int64_t index;
    uint32_t index_symbol_id;
    const char *index_symbol_name;
    const char *index_string;
    const char *field_name;
} CatchAggregateAlias;

typedef struct CatchCollectionViewAliases {
    bool empty;
    bool element;
    bool key_element;
    bool entry_key;
    bool entry_value;
} CatchCollectionViewAliases;

typedef struct CatchAliasState {
    bool binding_is_caught;
    uint32_t alias_ids[64];
    const char *alias_names[64];
    int alias_count;
    CatchAggregateAlias aggregate_aliases[64];
    int aggregate_alias_count;
} CatchAliasState;

typedef struct CatchCaptureState {
    bool has_caught;
    const char *catch_var;
    uint32_t catch_symbol_id;
    CatchAliasState alias_state;
    XaEffectSummary caught_summary;
} CatchCaptureState;

typedef struct FunctionReturnTargetEntry {
    uint32_t function_id;
    FunctionValueTarget target;
    bool seen;
    bool unknown;
} FunctionReturnTargetEntry;

typedef struct SpecializedParamTargetEntry {
    uint32_t function_id;
    uint32_t param_id;
    FunctionValueTarget target;
    bool unknown;
} SpecializedParamTargetEntry;

typedef struct FunctionExprCaptureEntry {
    AstNode *function_expr;
    FunctionValueAliasState state;
    CatchCaptureState catch_state;
} FunctionExprCaptureEntry;

#define ERROR_SET_FUNCTION_EXPR_WALK_MAX 128
#define ERROR_SET_AST_WALK_MAX 128

// Branch inference snapshots are tens of kilobytes. Heap storage prevents
// ordinary source nesting from multiplying them into the native thread stack.
// The shared depth guard makes excessive nesting conservatively incomplete.
typedef struct ErrorSetAliasSnapshot {
    FunctionValueAliasState function_values;
    CatchAliasState catch_aliases;
} ErrorSetAliasSnapshot;

typedef struct ErrorSetTryWalkState {
    XaEffectSummary try_summary;
    FunctionValueAliasState base_function_values;
    FunctionValueAliasState try_function_values;
    uint32_t try_mutation_ids[128];
    int try_mutation_count;
    CatchAliasState base_catch_aliases;
    CatchAliasState try_catch_aliases;
    CatchAliasState saved_catch_aliases;
} ErrorSetTryWalkState;

typedef struct ErrorSetBranchWalkState {
    FunctionValueAliasState base_function_values;
    FunctionValueAliasState then_function_values;
    FunctionValueAliasState else_function_values;
    CatchAliasState base_catch_aliases;
    CatchAliasState then_catch_aliases;
    CatchAliasState else_catch_aliases;
} ErrorSetBranchWalkState;

typedef struct ErrorSetLoopWalkState {
    FunctionValueAliasState base_function_values;
    FunctionValueAliasState iteration_function_values;
    CatchAliasState base_catch_aliases;
    CatchAliasState iteration_catch_aliases;
} ErrorSetLoopWalkState;

struct ErrorSetCtx {
    XaAnalyzer *analyzer;
    XaEffectSummary *current_summary; /* Effect summary being built for current function */
    const char *current_catch_var;    /* Catch variable currently in scope, if any */
    uint32_t current_catch_symbol_id; /* Symbol id for current_catch_var */
    bool current_catch_binding_is_caught;
    uint32_t current_catch_alias_ids[64];
    const char *current_catch_alias_names[64];
    int current_catch_alias_count;
    CatchAggregateAlias current_catch_aggregate_aliases[64];
    int current_catch_aggregate_alias_count;
    int current_catch_alias_control_depth;
    uint32_t function_value_alias_ids[128];
    FunctionValueTarget function_value_alias_targets[128];
    int function_value_alias_count;
    int function_value_control_depth;
    uint32_t function_value_mutation_ids[128];
    int function_value_mutation_count;
    int function_value_mutation_depth;
    FunctionReturnTargetEntry *function_return_targets;
    int function_return_target_count;
    int function_return_target_capacity;
    SpecializedParamTargetEntry *specialized_param_targets;
    int specialized_param_target_count;
    int specialized_param_target_capacity;
    FunctionExprCaptureEntry *function_expr_captures;
    int function_expr_capture_count;
    int function_expr_capture_capacity;
    AstNode *active_function_exprs[ERROR_SET_FUNCTION_EXPR_WALK_MAX];
    int active_function_expr_count;
    int ast_walk_depth; /* Shared statement/expression recursion budget. */
    int callsite_inline_depth;
    FunctionValueTarget current_return_target;
    bool current_return_target_seen;
    bool current_return_target_unknown;
    /* `var t = go f()` bindings: the task variable's symbol id and the spawn
     * expression it holds, so `await t` can charge the caller with the
     * coroutine body's errors. */
    uint32_t task_spawn_alias_ids[64];
    AstNode *task_spawn_alias_exprs[64];
    int task_spawn_alias_count;
    /* Depth of enclosing `linked scope` blocks in the function being walked.
     * A child spawned at depth > 0 re-raises its failure at the scope's exit,
     * so its errors belong to this function's error set. */
    int linked_scope_depth;
    XaEffectSummary *current_caught; /* Effect subset caught by current catch clause */
    XaSymbol *current_func;          /* Current function symbol */
    bool changed;                    /* Fixpoint: did anything change this iteration? */
};

/* Coroutine-boundary facts are per-body.  Expanding a callee's body into this
 * summary must not carry them in: a `go` inside the callee is detached whatever
 * scope surrounds the call site, and the callee's own task bindings die with
 * it.  Save on entry, restore on exit. */
typedef struct CoroBoundaryState {
    int task_spawn_alias_count;
    int linked_scope_depth;
} CoroBoundaryState;

static void enter_callee_body_coro_boundary(ErrorSetCtx *ctx, CoroBoundaryState *state) {
    state->task_spawn_alias_count = ctx->task_spawn_alias_count;
    state->linked_scope_depth = ctx->linked_scope_depth;
    ctx->linked_scope_depth = 0;
}

static void leave_callee_body_coro_boundary(ErrorSetCtx *ctx, const CoroBoundaryState *state) {
    ctx->task_spawn_alias_count = state->task_spawn_alias_count;
    ctx->linked_scope_depth = state->linked_scope_depth;
}

static void capture_catch_alias_state(ErrorSetCtx *ctx, CatchAliasState *state);
static void restore_catch_alias_state(ErrorSetCtx *ctx, const CatchAliasState *state);
static int current_catch_aggregate_alias_index(ErrorSetCtx *ctx, const CatchAggregateAlias *alias);
static bool literal_i64_index(AstNode *expr, int64_t *out);
static bool catch_aggregate_index_key(ErrorSetCtx *ctx, AstNode *expr, int64_t *literal_index,
                                      uint32_t *symbol_id, const char **symbol_name,
                                      const char **string_key);
static bool variable_ref_symbol(ErrorSetCtx *ctx, AstNode *expr, uint32_t *symbol_id,
                                const char **name);

static bool es_summary_add_enum_selection(ErrorSetCtx *ctx, const XaSelection *sel) {
    if (!ctx || !sel || sel->kind != XA_SEL_ENUM_MEMBER)
        return false;
    XrType *enum_type = sel->result_type;
    if ((!enum_type || !XR_TYPE_IS_ENUM(enum_type)) && sel->target_symbol)
        enum_type = sel->target_symbol->links.type;
    if (!enum_type || !XR_TYPE_IS_ENUM(enum_type))
        return false;
    if (sel->field_index >= 0)
        es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary, enum_type,
                                 (uint32_t) sel->field_index);
    else
        es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary, enum_type);
    return true;
}

/* ========== Forward Declarations ========== */

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_stmt_inner(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_expr_inner(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_block(ErrorSetCtx *ctx, AstNode *node);
static FunctionValueTarget resolve_call_target_depth(ErrorSetCtx *ctx, AstNode *callee, int depth);
static bool function_value_target_add(FunctionValueTarget *target, XaSymbol *sym,
                                      AstNode *function_expr);

static bool function_has_error_diagnostic(ErrorSetCtx *ctx, AstNode *func_node,
                                          XaSymbol *func_sym) {
    if (!ctx || !ctx->analyzer || !func_node || !func_sym)
        return false;
    const char *function_file = func_sym->links.file_path;
    int first_line = func_node->line;
    int last_line = func_node->end_line > 0 ? func_node->end_line : first_line;
    for (XaDiagnostic *diag = ctx->analyzer->diagnostics; diag; diag = diag->next) {
        if (diag->severity != XR_DIAG_SEV_ERROR)
            continue;
        const char *diagnostic_file = diag->location.file;
        if (function_file && diagnostic_file && strcmp(function_file, diagnostic_file) != 0)
            continue;
        if ((int) diag->location.line >= first_line && (int) diag->location.line <= last_line)
            return true;
    }
    return false;
}

/* ========== Helpers ========== */

static XaSymbol *resolve_func_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_METHOD_DECL) {
        XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
        if (scope && scope->function_symbol &&
            (scope->function_symbol->kind == XA_SYM_FUNCTION ||
             scope->function_symbol->kind == XA_SYM_METHOD))
            return scope->function_symbol;
        return NULL;
    }
    if (node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
        if (scope && scope->function_symbol &&
            (scope->function_symbol->kind == XA_SYM_FUNCTION ||
             scope->function_symbol->kind == XA_SYM_METHOD))
            return scope->function_symbol;
        if (fn->symbol_id != 0) {
            XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, fn->symbol_id);
            if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD))
                return sym;
        }
        const char *name = fn->name;
        if (name) {
            XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
            if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD))
                return sym;
        }
    }
    return NULL;
}

static AstNode *function_like_body(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.body;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.body;
    if (node->type == AST_FUNCTION_EXPR)
        return node->as.function_expr.body;
    return NULL;
}

static AstNode *identity_source(AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static XaSymbol *lookup_variable_symbol(XaAnalyzer *analyzer, AstNode *node) {
    node = identity_source(node);
    if (!analyzer || !node || node->type != AST_VARIABLE || !node->as.variable.name)
        return NULL;

    if (node->as.variable.symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, node->as.variable.symbol_id);
        if (sym)
            return sym;
    }

    XaSymbol *sym = xa_analyzer_lookup(analyzer, node->as.variable.name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, node->as.variable.name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, node->as.variable.name);
    return sym;
}

static bool symbol_has_function_type(XaSymbol *sym) {
    if (!sym)
        return false;
    if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)
        return true;
    XrType *type = sym->links.type;
    return type && XR_TYPE_IS_FUNCTION(type);
}

static FunctionValueTarget function_value_target_none(void) {
    FunctionValueTarget target;
    target.symbol = NULL;
    target.function_expr = NULL;
    memset(target.target_symbols, 0, sizeof(target.target_symbols));
    memset(target.target_function_exprs, 0, sizeof(target.target_function_exprs));
    target.target_count = 0;
    return target;
}

static FunctionValueTarget function_value_target_symbol(XaSymbol *sym) {
    FunctionValueTarget target = function_value_target_none();
    target.symbol = sym;
    if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)) {
        target.target_symbols[0] = sym;
        target.target_count = 1;
    }
    return target;
}

static FunctionValueTarget function_value_target_expr(AstNode *function_expr) {
    FunctionValueTarget target = function_value_target_none();
    target.function_expr = function_expr;
    if (function_expr) {
        target.target_function_exprs[0] = function_expr;
        target.target_count = 1;
    }
    return target;
}

static bool function_value_target_is_exact(FunctionValueTarget target) {
    return target.target_count > 0;
}

static const XaMethodSlot *find_method_slot_for_symbol(const XrClassInfo *info,
                                                       const XaSymbol *method) {
    if (!info || !method || !info->vtable)
        return NULL;
    for (int i = 0; i < info->vtable_size; i++) {
        const XaMethodSlot *slot = &info->vtable[i];
        if (slot->symbol == method)
            return slot;
    }
    if (!method->name)
        return NULL;
    for (int i = 0; i < info->vtable_size; i++) {
        const XaMethodSlot *slot = &info->vtable[i];
        if (slot->symbol && slot->symbol->name && strcmp(slot->symbol->name, method->name) == 0)
            return slot;
    }
    return NULL;
}

static bool method_selection_has_open_virtual_dispatch(const XaSelection *sel) {
    if (!sel || sel->kind != XA_SEL_METHOD || !sel->target_symbol)
        return false;
    XrType *receiver_type = sel->receiver_type;
    if (!receiver_type)
        return false;
    if (receiver_type->kind == XR_KIND_INTERFACE)
        return true;
    if (!XR_TYPE_IS_INSTANCE(receiver_type))
        return false;

    XrClassInfo *info = receiver_type->instance.class_ref;
    if (!info || info->explicit_final || sel->target_symbol->is_static)
        return false;

    const XaMethodSlot *slot = find_method_slot_for_symbol(info, sel->target_symbol);
    if (slot)
        return !slot->is_final;
    return info->has_subclass;
}

static bool class_info_is_or_extends(const XrClassInfo *candidate, const XrClassInfo *base) {
    if (!candidate || !base)
        return false;
    for (const XrClassInfo *cur = candidate; cur; cur = cur->base) {
        if (cur == base)
            return true;
    }
    return false;
}

static bool class_info_implements_interface(const XrClassInfo *info, const XrType *interface_type) {
    if (!info || !interface_type || interface_type->kind != XR_KIND_INTERFACE)
        return false;
    const char *target_name = interface_type->instance.class_name;
    if (!target_name)
        return false;

    int target_args = interface_type->instance.type_arg_count;
    for (const XrClassInfo *cur = info; cur; cur = cur->base) {
        for (int i = 0; i < cur->interface_count; i++) {
            XrType *iface = cur->interface_types ? cur->interface_types[i] : NULL;
            if (!iface || !iface->instance.class_name)
                continue;
            if (strcmp(iface->instance.class_name, target_name) != 0)
                continue;
            if (target_args == 0)
                return true;
            if (iface->instance.type_arg_count != target_args)
                continue;
            bool args_match = true;
            for (int j = 0; j < target_args; j++) {
                XrType *target_arg = interface_type->instance.type_args[j];
                XrType *candidate_arg = iface->instance.type_args[j];
                if (!target_arg || !candidate_arg)
                    continue;
                if (XR_TYPE_IS_UNKNOWN(target_arg) || XR_TYPE_IS_UNKNOWN(candidate_arg))
                    continue;
                if (target_arg->kind == XR_KIND_TYPE_PARAM ||
                    candidate_arg->kind == XR_KIND_TYPE_PARAM)
                    continue;
                if (!xr_type_assignable(target_arg, candidate_arg)) {
                    args_match = false;
                    break;
                }
            }
            if (args_match)
                return true;
        }
    }
    return false;
}

static XaSymbol *method_target_for_dispatch_class(const XaSelection *sel, XrClassInfo *class_info) {
    if (!sel || !sel->target_symbol || !sel->target_symbol->name || !class_info)
        return NULL;

    if (sel->receiver_type && XR_TYPE_IS_INSTANCE(sel->receiver_type)) {
        XrClassInfo *receiver_info = sel->receiver_type->instance.class_ref;
        const XaMethodSlot *receiver_slot =
            find_method_slot_for_symbol(receiver_info, sel->target_symbol);
        if (receiver_slot && receiver_slot->vtable_index >= 0 && class_info->vtable &&
            receiver_slot->vtable_index < class_info->vtable_size) {
            XaSymbol *slot_symbol = class_info->vtable[receiver_slot->vtable_index].symbol;
            if (slot_symbol && slot_symbol->name &&
                strcmp(slot_symbol->name, sel->target_symbol->name) == 0)
                return slot_symbol;
        }
    }

    return xa_class_info_lookup_instance_member(class_info, sel->target_symbol->name);
}

typedef struct OpenDispatchTargetCollector {
    ErrorSetCtx *ctx;
    const XaSelection *selection;
    FunctionValueTarget target;
    bool exact;
} OpenDispatchTargetCollector;

static void collect_open_dispatch_candidate(OpenDispatchTargetCollector *collector,
                                            XaSymbol *class_sym) {
    if (!collector || !collector->ctx || !collector->selection || !class_sym ||
        class_sym->kind != XA_SYM_CLASS || !collector->exact)
        return;

    XaSymbolLinks *links = xa_analyzer_get_links(collector->ctx->analyzer, class_sym);
    XrClassInfo *class_info = links ? links->class_info : NULL;
    XrType *class_type = links ? links->type : NULL;
    if (!class_info || (class_type && class_type->kind == XR_KIND_INTERFACE))
        return;

    XrType *receiver_type = collector->selection->receiver_type;
    bool matches = false;
    if (receiver_type && receiver_type->kind == XR_KIND_INTERFACE) {
        matches = class_info_implements_interface(class_info, receiver_type);
    } else if (receiver_type && XR_TYPE_IS_INSTANCE(receiver_type)) {
        matches = class_info_is_or_extends(class_info, receiver_type->instance.class_ref);
    }
    if (!matches)
        return;

    XaSymbol *method = method_target_for_dispatch_class(collector->selection, class_info);
    if (!method || method->kind != XA_SYM_METHOD)
        return;
    if (!function_value_target_add(&collector->target, method, NULL))
        collector->exact = false;
}

static void collect_open_dispatch_targets_from_scope(OpenDispatchTargetCollector *collector,
                                                     XaScope *scope) {
    if (!collector || !scope || !collector->exact)
        return;

    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    for (int i = 0; i < count && collector->exact; i++)
        collect_open_dispatch_candidate(collector, symbols[i]);
    if (symbols)
        xr_free(symbols);

    for (int i = 0; i < scope->child_count && collector->exact; i++)
        collect_open_dispatch_targets_from_scope(collector,
                                                 scope->children ? scope->children[i] : NULL);
}

static FunctionValueTarget
resolve_open_virtual_dispatch_targets(ErrorSetCtx *ctx, const XaSelection *sel, bool *exact_out) {
    if (exact_out)
        *exact_out = false;
    if (!ctx || !ctx->analyzer || !ctx->analyzer->global_scope ||
        !method_selection_has_open_virtual_dispatch(sel))
        return function_value_target_none();

    OpenDispatchTargetCollector collector = {
        .ctx = ctx,
        .selection = sel,
        .target = function_value_target_none(),
        .exact = true,
    };
    collect_open_dispatch_targets_from_scope(&collector, ctx->analyzer->global_scope);
    if (!collector.exact || !function_value_target_is_exact(collector.target))
        return function_value_target_none();
    if (exact_out)
        *exact_out = true;
    return collector.target;
}

static bool function_value_target_equal(FunctionValueTarget a, FunctionValueTarget b) {
    if (a.target_count != b.target_count)
        return false;
    for (int i = 0; i < a.target_count; i++) {
        bool found = false;
        for (int j = 0; j < b.target_count; j++) {
            if (a.target_symbols[i] == b.target_symbols[j] &&
                a.target_function_exprs[i] == b.target_function_exprs[j]) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool function_value_target_add(FunctionValueTarget *target, XaSymbol *sym,
                                      AstNode *function_expr) {
    if (!target || (!sym && !function_expr))
        return false;
    for (int i = 0; i < target->target_count; i++) {
        if (target->target_symbols[i] == sym && target->target_function_exprs[i] == function_expr)
            return true;
    }
    if (target->target_count >= 8)
        return false;
    int slot = target->target_count++;
    target->target_symbols[slot] = sym;
    target->target_function_exprs[slot] = function_expr;
    if (slot == 0) {
        target->symbol = sym;
        target->function_expr = function_expr;
    }
    return true;
}

static FunctionValueTarget function_value_target_merge(FunctionValueTarget a,
                                                       FunctionValueTarget b) {
    FunctionValueTarget merged = function_value_target_none();
    if (!function_value_target_is_exact(a) || !function_value_target_is_exact(b))
        return merged;
    for (int i = 0; i < a.target_count; i++) {
        if (!function_value_target_add(&merged, a.target_symbols[i], a.target_function_exprs[i]))
            return function_value_target_none();
    }
    for (int i = 0; i < b.target_count; i++) {
        if (!function_value_target_add(&merged, b.target_symbols[i], b.target_function_exprs[i]))
            return function_value_target_none();
    }
    return merged;
}

static bool expr_has_function_type(ErrorSetCtx *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr)
        return false;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    return type && XR_TYPE_IS_FUNCTION(type);
}

static bool current_function_returns_function(ErrorSetCtx *ctx) {
    if (!ctx || !ctx->current_func)
        return false;
    XaSymbolLinks *links = &ctx->current_func->links;
    XrType *return_type = links->return_type;
    if (!return_type && links->type && XR_TYPE_IS_FUNCTION(links->type))
        return_type = links->type->function.return_type;
    return return_type && XR_TYPE_IS_FUNCTION(return_type);
}

static void record_current_function_return_target(ErrorSetCtx *ctx, FunctionValueTarget target,
                                                  bool unknown) {
    if (!ctx || !ctx->current_func)
        return;
    ctx->current_return_target_seen = true;
    if (unknown || !function_value_target_is_exact(target)) {
        ctx->current_return_target = function_value_target_none();
        ctx->current_return_target_unknown = true;
        return;
    }
    if (!function_value_target_is_exact(ctx->current_return_target)) {
        ctx->current_return_target = target;
        return;
    }
    FunctionValueTarget merged = function_value_target_merge(ctx->current_return_target, target);
    if (!function_value_target_is_exact(merged)) {
        ctx->current_return_target = function_value_target_none();
        ctx->current_return_target_unknown = true;
        return;
    }
    ctx->current_return_target = merged;
}

static FunctionReturnTargetEntry *lookup_function_return_target_entry(ErrorSetCtx *ctx,
                                                                      XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return NULL;
    for (int i = 0; i < ctx->function_return_target_count; i++) {
        if (ctx->function_return_targets[i].function_id == sym->id)
            return &ctx->function_return_targets[i];
    }
    return NULL;
}

static FunctionValueTarget lookup_function_return_target(ErrorSetCtx *ctx, XaSymbol *sym,
                                                         bool *seen, bool *unknown) {
    if (seen)
        *seen = false;
    if (unknown)
        *unknown = false;
    FunctionReturnTargetEntry *entry = lookup_function_return_target_entry(ctx, sym);
    if (!entry)
        return function_value_target_none();
    if (seen)
        *seen = entry->seen;
    if (unknown)
        *unknown = entry->unknown;
    return entry->target;
}

static void store_function_return_target(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return;
    bool seen = ctx->current_return_target_seen;
    bool unknown = ctx->current_return_target_unknown;
    FunctionValueTarget target =
        unknown ? function_value_target_none() : ctx->current_return_target;
    if (!seen && !unknown)
        return;

    FunctionReturnTargetEntry *entry = lookup_function_return_target_entry(ctx, sym);
    if (!entry) {
        if (ctx->function_return_target_count >= ctx->function_return_target_capacity) {
            int new_cap = ctx->function_return_target_capacity == 0
                              ? 32
                              : ctx->function_return_target_capacity * 2;
            XR_REALLOC_OR_ABORT(ctx->function_return_targets,
                                (size_t) new_cap * sizeof(FunctionReturnTargetEntry),
                                "function return target grow");
            ctx->function_return_target_capacity = new_cap;
        }
        entry = &ctx->function_return_targets[ctx->function_return_target_count++];
        memset(entry, 0, sizeof(*entry));
        entry->function_id = sym->id;
        ctx->changed = true;
    } else if (entry->seen != seen || entry->unknown != unknown ||
               !function_value_target_equal(entry->target, target)) {
        ctx->changed = true;
    }

    entry->seen = seen;
    entry->unknown = unknown;
    entry->target = target;
}

static bool is_mono_specialized_function_symbol(XaSymbol *sym) {
    if (!sym || sym->kind != XA_SYM_FUNCTION || !sym->name || !strchr(sym->name, '$'))
        return false;
    AstNode *node = sym->links.function_decl_node;
    return node && node->type == AST_FUNCTION_DECL && node->as.function_decl.type_param_count == 0;
}

static SpecializedParamTargetEntry *
lookup_specialized_param_target_entry(ErrorSetCtx *ctx, XaSymbol *func_sym, XaSymbol *param_sym) {
    if (!ctx || !func_sym || !param_sym || func_sym->id == 0 || param_sym->id == 0)
        return NULL;
    for (int i = 0; i < ctx->specialized_param_target_count; i++) {
        SpecializedParamTargetEntry *entry = &ctx->specialized_param_targets[i];
        if (entry->function_id == func_sym->id && entry->param_id == param_sym->id)
            return entry;
    }
    return NULL;
}

static SpecializedParamTargetEntry *
ensure_specialized_param_target_entry(ErrorSetCtx *ctx, XaSymbol *func_sym, XaSymbol *param_sym) {
    SpecializedParamTargetEntry *entry =
        lookup_specialized_param_target_entry(ctx, func_sym, param_sym);
    if (entry)
        return entry;
    if (!ctx || !func_sym || !param_sym || func_sym->id == 0 || param_sym->id == 0)
        return NULL;
    if (ctx->specialized_param_target_count >= ctx->specialized_param_target_capacity) {
        int new_cap = ctx->specialized_param_target_capacity == 0
                          ? 32
                          : ctx->specialized_param_target_capacity * 2;
        XR_REALLOC_OR_ABORT(ctx->specialized_param_targets,
                            (size_t) new_cap * sizeof(SpecializedParamTargetEntry),
                            "specialized param target grow");
        ctx->specialized_param_target_capacity = new_cap;
    }
    entry = &ctx->specialized_param_targets[ctx->specialized_param_target_count++];
    memset(entry, 0, sizeof(*entry));
    entry->function_id = func_sym->id;
    entry->param_id = param_sym->id;
    entry->target = function_value_target_none();
    ctx->changed = true;
    return entry;
}

static void record_specialized_param_target(ErrorSetCtx *ctx, XaSymbol *func_sym,
                                            XaSymbol *param_sym, FunctionValueTarget target,
                                            bool unknown) {
    if (!ctx || !is_mono_specialized_function_symbol(func_sym) || !param_sym ||
        !symbol_has_function_type(param_sym))
        return;
    SpecializedParamTargetEntry *entry =
        ensure_specialized_param_target_entry(ctx, func_sym, param_sym);
    if (!entry)
        return;

    if (unknown || !function_value_target_is_exact(target)) {
        if (!entry->unknown || function_value_target_is_exact(entry->target)) {
            entry->unknown = true;
            entry->target = function_value_target_none();
            ctx->changed = true;
        }
        return;
    }
    if (entry->unknown)
        return;
    if (!function_value_target_is_exact(entry->target)) {
        entry->target = target;
        ctx->changed = true;
        return;
    }
    FunctionValueTarget merged = function_value_target_merge(entry->target, target);
    if (!function_value_target_is_exact(merged)) {
        entry->unknown = true;
        entry->target = function_value_target_none();
        ctx->changed = true;
        return;
    }
    if (!function_value_target_equal(entry->target, merged)) {
        entry->target = merged;
        ctx->changed = true;
    }
}

static XaSymbol *lookup_symbol_by_id(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || !ctx->analyzer || symbol_id == 0)
        return NULL;
    return xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
}

static XaSymbol *lookup_assignment_symbol(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !ctx->analyzer || !assign)
        return NULL;
    XaSymbol *sym = lookup_symbol_by_id(ctx, assign->symbol_id);
    if (sym || !assign->name)
        return sym;
    sym = xa_analyzer_lookup(ctx->analyzer, assign->name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(ctx->analyzer, assign->name, ctx->analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(ctx->analyzer, assign->name);
    return sym;
}

static XaSymbol *resolve_function_alias_target(XaAnalyzer *analyzer, XaSymbol *sym, int depth) {
    if (!analyzer || !sym)
        return NULL;
    if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)
        return sym;
    if (depth >= 32 || !sym->is_const || sym->is_rebindable || !sym->links.const_initializer)
        return sym;
    if (!symbol_has_function_type(sym))
        return sym;

    AstNode *source = identity_source(sym->links.const_initializer);
    if (!source || source->type != AST_VARIABLE)
        return sym;

    XaSymbol *source_sym = lookup_variable_symbol(analyzer, source);
    if (!source_sym || source_sym == sym)
        return sym;
    return resolve_function_alias_target(analyzer, source_sym, depth + 1);
}

static FunctionValueTarget lookup_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return function_value_target_none();
    for (int i = ctx->function_value_alias_count - 1; i >= 0; i--) {
        if (ctx->function_value_alias_ids[i] == sym->id)
            return ctx->function_value_alias_targets[i];
    }
    return function_value_target_none();
}

static void set_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym,
                                            FunctionValueTarget target) {
    if (!ctx || !sym || sym->id == 0)
        return;
    for (int i = 0; i < ctx->function_value_alias_count; i++) {
        if (ctx->function_value_alias_ids[i] == sym->id) {
            ctx->function_value_alias_targets[i] = target;
            return;
        }
    }
    if (ctx->function_value_alias_count >= 128)
        return;
    int slot = ctx->function_value_alias_count++;
    ctx->function_value_alias_ids[slot] = sym->id;
    ctx->function_value_alias_targets[slot] = target;
}

static void invalidate_function_value_alias_target(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || symbol_id == 0)
        return;
    for (int i = 0; i < ctx->function_value_alias_count; i++) {
        if (ctx->function_value_alias_ids[i] == symbol_id)
            ctx->function_value_alias_targets[i] = function_value_target_none();
    }
}

static void capture_function_value_alias_state(ErrorSetCtx *ctx, FunctionValueAliasState *state) {
    if (!ctx || !state)
        return;
    state->count = ctx->function_value_alias_count;
    for (int i = 0; i < state->count; i++) {
        state->ids[i] = ctx->function_value_alias_ids[i];
        state->targets[i] = ctx->function_value_alias_targets[i];
    }
}

static void restore_function_value_alias_state(ErrorSetCtx *ctx,
                                               const FunctionValueAliasState *state) {
    if (!ctx || !state)
        return;
    ctx->function_value_alias_count = state->count;
    for (int i = 0; i < state->count; i++) {
        ctx->function_value_alias_ids[i] = state->ids[i];
        ctx->function_value_alias_targets[i] = state->targets[i];
    }
}

static bool function_value_alias_state_equal(const FunctionValueAliasState *a,
                                             const FunctionValueAliasState *b) {
    if (!a || !b || a->count != b->count)
        return false;
    for (int i = 0; i < a->count; i++) {
        if (a->ids[i] != b->ids[i] || !function_value_target_equal(a->targets[i], b->targets[i]))
            return false;
    }
    return true;
}

static FunctionValueTarget state_lookup_function_value_target(const FunctionValueAliasState *state,
                                                              uint32_t symbol_id) {
    if (!state || symbol_id == 0)
        return function_value_target_none();
    for (int i = state->count - 1; i >= 0; i--) {
        if (state->ids[i] == symbol_id)
            return state->targets[i];
    }
    return function_value_target_none();
}

static bool function_value_alias_id_present(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || symbol_id == 0)
        return false;
    for (int i = ctx->function_value_alias_count - 1; i >= 0; i--) {
        if (ctx->function_value_alias_ids[i] == symbol_id)
            return true;
    }
    return false;
}

static FunctionExprCaptureEntry *lookup_function_expr_capture_entry(ErrorSetCtx *ctx,
                                                                    AstNode *function_expr) {
    if (!ctx || !function_expr)
        return NULL;
    for (int i = 0; i < ctx->function_expr_capture_count; i++) {
        if (ctx->function_expr_captures[i].function_expr == function_expr)
            return &ctx->function_expr_captures[i];
    }
    return NULL;
}

static bool names_equal(const char *a, const char *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

static bool bitset_equal(const XaBitSet *a, const XaBitSet *b) {
    if (!a || !b || a->word_count != b->word_count)
        return false;
    for (uint32_t i = 0; i < a->word_count; i++) {
        uint64_t lhs = a->words ? a->words[i] : 0;
        uint64_t rhs = b->words ? b->words[i] : 0;
        if (lhs != rhs)
            return false;
    }
    return true;
}

static bool effect_summary_equal(const XaEffectSummary *a, const XaEffectSummary *b) {
    if (!a || !b || a->completeness != b->completeness ||
        a->unknown_reasons != b->unknown_reasons || a->semantic_effects != b->semantic_effects ||
        a->unknown_semantic_effects != b->unknown_semantic_effects ||
        a->error_set_completeness != b->error_set_completeness ||
        a->error_unknown_reasons != b->error_unknown_reasons ||
        a->contains_unsafe_op != b->contains_unsafe_op ||
        a->requires_unsafe_at_call != b->requires_unsafe_at_call ||
        a->escaping.count != b->escaping.count || a->root_count != b->root_count)
        return false;
    for (uint32_t i = 0; i < a->escaping.count; i++) {
        const XaErrorTypeSet *lhs = &a->escaping.types[i];
        const XaErrorTypeSet *rhs = &b->escaping.types[i];
        if (lhs->type_id != rhs->type_id || lhs->stable_type_key != rhs->stable_type_key ||
            lhs->all_variants != rhs->all_variants || !bitset_equal(&lhs->variants, &rhs->variants))
            return false;
    }
    for (uint32_t i = 0; i < a->root_count; i++) {
        if (a->roots[i] != b->roots[i])
            return false;
    }
    return true;
}

static bool catch_alias_state_equal(const CatchAliasState *a, const CatchAliasState *b) {
    if (!a || !b || a->binding_is_caught != b->binding_is_caught ||
        a->alias_count != b->alias_count || a->aggregate_alias_count != b->aggregate_alias_count)
        return false;
    for (int i = 0; i < a->alias_count; i++) {
        if (a->alias_ids[i] != b->alias_ids[i] ||
            !names_equal(a->alias_names[i], b->alias_names[i]))
            return false;
    }
    for (int i = 0; i < a->aggregate_alias_count; i++) {
        const CatchAggregateAlias *lhs = &a->aggregate_aliases[i];
        const CatchAggregateAlias *rhs = &b->aggregate_aliases[i];
        if (lhs->container_id != rhs->container_id ||
            !names_equal(lhs->container_name, rhs->container_name) || lhs->kind != rhs->kind ||
            lhs->index != rhs->index || !names_equal(lhs->field_name, rhs->field_name))
            return false;
    }
    return true;
}

static void catch_capture_state_init(CatchCaptureState *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    xa_effect_summary_init(&state->caught_summary);
}

static void catch_capture_state_clear(CatchCaptureState *state) {
    if (!state)
        return;
    xa_effect_summary_clear(&state->caught_summary);
    memset(state, 0, sizeof(*state));
}

static void capture_current_catch_capture_state(ErrorSetCtx *ctx, CatchCaptureState *state) {
    catch_capture_state_init(state);
    if (!ctx || !state || !ctx->current_caught)
        return;
    state->has_caught = true;
    state->catch_var = ctx->current_catch_var;
    state->catch_symbol_id = ctx->current_catch_symbol_id;
    capture_catch_alias_state(ctx, &state->alias_state);
    xa_effect_summary_add_summary(ctx->analyzer->effect_db, &state->caught_summary,
                                  ctx->current_caught);
}

static bool catch_capture_state_equal(const CatchCaptureState *a, const CatchCaptureState *b) {
    if (!a || !b || a->has_caught != b->has_caught)
        return false;
    if (!a->has_caught)
        return true;
    return a->catch_symbol_id == b->catch_symbol_id && names_equal(a->catch_var, b->catch_var) &&
           catch_alias_state_equal(&a->alias_state, &b->alias_state) &&
           effect_summary_equal(&a->caught_summary, &b->caught_summary);
}

static void record_function_expr_capture(ErrorSetCtx *ctx, AstNode *function_expr) {
    if (!ctx || !function_expr || function_expr->type != AST_FUNCTION_EXPR)
        return;
    FunctionValueAliasState state;
    capture_function_value_alias_state(ctx, &state);
    CatchCaptureState catch_state;
    capture_current_catch_capture_state(ctx, &catch_state);

    FunctionExprCaptureEntry *entry = lookup_function_expr_capture_entry(ctx, function_expr);
    if (!entry) {
        if (ctx->function_expr_capture_count >= ctx->function_expr_capture_capacity) {
            int new_cap = ctx->function_expr_capture_capacity == 0
                              ? 32
                              : ctx->function_expr_capture_capacity * 2;
            XR_REALLOC_OR_ABORT(ctx->function_expr_captures,
                                (size_t) new_cap * sizeof(FunctionExprCaptureEntry),
                                "function expr capture grow");
            ctx->function_expr_capture_capacity = new_cap;
        }
        entry = &ctx->function_expr_captures[ctx->function_expr_capture_count++];
        memset(entry, 0, sizeof(*entry));
        entry->function_expr = function_expr;
        ctx->changed = true;
    } else if (!function_value_alias_state_equal(&entry->state, &state) ||
               !catch_capture_state_equal(&entry->catch_state, &catch_state)) {
        ctx->changed = true;
    }
    entry->state = state;
    catch_capture_state_clear(&entry->catch_state);
    entry->catch_state = catch_state;
}

static void apply_function_expr_capture(ErrorSetCtx *ctx, AstNode *function_expr) {
    FunctionExprCaptureEntry *entry = lookup_function_expr_capture_entry(ctx, function_expr);
    if (!ctx || !entry)
        return;
    for (int i = 0; i < entry->state.count; i++) {
        uint32_t id = entry->state.ids[i];
        if (id == 0 || function_value_alias_id_present(ctx, id))
            continue;
        FunctionValueTarget target = entry->state.targets[i];
        if (!function_value_target_is_exact(target))
            continue;
        XaSymbol *sym = lookup_symbol_by_id(ctx, id);
        if (!sym || !symbol_has_function_type(sym))
            continue;
        set_function_value_alias_target(ctx, sym, target);
    }
}

static bool apply_function_expr_catch_capture(ErrorSetCtx *ctx, AstNode *function_expr) {
    FunctionExprCaptureEntry *entry = lookup_function_expr_capture_entry(ctx, function_expr);
    if (!ctx || !entry || !entry->catch_state.has_caught)
        return false;
    ctx->current_catch_var = entry->catch_state.catch_var;
    ctx->current_catch_symbol_id = entry->catch_state.catch_symbol_id;
    restore_catch_alias_state(ctx, &entry->catch_state.alias_state);
    ctx->current_caught = &entry->catch_state.caught_summary;
    ctx->current_catch_alias_control_depth = 0;
    return true;
}

static void clear_function_expr_captures(ErrorSetCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->function_expr_capture_count; i++)
        catch_capture_state_clear(&ctx->function_expr_captures[i].catch_state);
    xr_free(ctx->function_expr_captures);
    ctx->function_expr_captures = NULL;
    ctx->function_expr_capture_count = 0;
    ctx->function_expr_capture_capacity = 0;
}

static void merge_function_value_path_states(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                             const FunctionValueAliasState **path_states,
                                             int path_count) {
    if (!ctx || !base || !path_states || path_count <= 0)
        return;
    restore_function_value_alias_state(ctx, base);

    uint32_t ids[128];
    int count = 0;
    for (int s = -1; s < path_count; s++) {
        const FunctionValueAliasState *state = s < 0 ? base : path_states[s];
        if (!state)
            continue;
        for (int i = 0; i < state->count; i++) {
            uint32_t id = state->ids[i];
            if (id == 0)
                continue;
            bool seen = false;
            for (int j = 0; j < count; j++) {
                if (ids[j] == id) {
                    seen = true;
                    break;
                }
            }
            if (!seen && count < 128)
                ids[count++] = id;
        }
    }

    for (int i = 0; i < count; i++) {
        FunctionValueTarget merged = state_lookup_function_value_target(path_states[0], ids[i]);
        for (int p = 1; p < path_count && function_value_target_is_exact(merged); p++) {
            merged = function_value_target_merge(
                merged, state_lookup_function_value_target(path_states[p], ids[i]));
        }
        invalidate_function_value_alias_target(ctx, ids[i]);
        if (!function_value_target_is_exact(merged))
            continue;
        XaSymbol *sym = lookup_symbol_by_id(ctx, ids[i]);
        if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
            !symbol_has_function_type(sym))
            continue;
        set_function_value_alias_target(ctx, sym, merged);
    }
}

static void merge_function_value_if_states(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                           const FunctionValueAliasState *then_state,
                                           const FunctionValueAliasState *else_state) {
    const FunctionValueAliasState *paths[2] = {then_state, else_state};
    merge_function_value_path_states(ctx, base, paths, 2);
}

static void merge_function_value_loop_state(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                            const FunctionValueAliasState *iteration_state) {
    const FunctionValueAliasState *paths[2] = {base, iteration_state};
    merge_function_value_path_states(ctx, base, paths, 2);
}

static void track_function_value_alias_mutation(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || ctx->function_value_mutation_depth <= 0 || !sym || sym->id == 0 ||
        sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    for (int i = 0; i < ctx->function_value_mutation_count; i++) {
        if (ctx->function_value_mutation_ids[i] == sym->id)
            return;
    }
    if (ctx->function_value_mutation_count >= 128)
        return;
    ctx->function_value_mutation_ids[ctx->function_value_mutation_count++] = sym->id;
}

static void restore_function_value_alias_state_for_catch_entry(ErrorSetCtx *ctx,
                                                               const FunctionValueAliasState *base,
                                                               const uint32_t *try_mutation_ids,
                                                               int try_mutation_count) {
    restore_function_value_alias_state(ctx, base);
    for (int i = 0; i < try_mutation_count; i++)
        invalidate_function_value_alias_target(ctx, try_mutation_ids[i]);
}

static FunctionValueTarget
resolve_returned_function_value_call_target(ErrorSetCtx *ctx, AstNode *call_expr, int depth) {
    if (!ctx || !call_expr || call_expr->type != AST_CALL_EXPR || depth >= 32)
        return function_value_target_none();
    FunctionValueTarget callee_target =
        resolve_call_target_depth(ctx, call_expr->as.call_expr.callee, depth + 1);
    if (!function_value_target_is_exact(callee_target))
        return function_value_target_none();

    FunctionValueTarget returned = function_value_target_none();
    for (int i = 0; i < callee_target.target_count; i++) {
        XaSymbol *callee_sym = callee_target.target_symbols[i];
        if (!callee_sym)
            return function_value_target_none();
        bool seen = false;
        bool unknown = false;
        FunctionValueTarget target =
            lookup_function_return_target(ctx, callee_sym, &seen, &unknown);
        if (!seen || unknown || !function_value_target_is_exact(target))
            return function_value_target_none();
        returned = function_value_target_is_exact(returned)
                       ? function_value_target_merge(returned, target)
                       : target;
        if (!function_value_target_is_exact(returned))
            return function_value_target_none();
    }
    return returned;
}

static FunctionValueTarget resolve_function_value_expr_target(ErrorSetCtx *ctx, AstNode *expr,
                                                              int depth) {
    if (!ctx || depth >= 32)
        return function_value_target_none();
    expr = identity_source(expr);
    if (!expr)
        return function_value_target_none();
    if (expr->type == AST_FUNCTION_EXPR) {
        record_function_expr_capture(ctx, expr);
        return function_value_target_expr(expr);
    }
    if (expr->type == AST_CALL_EXPR) {
        FunctionValueTarget returned =
            resolve_returned_function_value_call_target(ctx, expr, depth + 1);
        if (function_value_target_is_exact(returned))
            return returned;
        return function_value_target_none();
    }
    const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, expr);
    if (sel && sel->target_symbol &&
        (sel->kind == XA_SEL_MODULE_EXPORT || sel->kind == XA_SEL_STATIC_MEMBER)) {
        XaSymbol *selected = sel->target_symbol;
        if (selected->kind == XA_SYM_FUNCTION || selected->kind == XA_SYM_METHOD)
            return function_value_target_symbol(selected);
    }
    if (expr->type != AST_VARIABLE)
        return function_value_target_none();
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, expr);
    XaSymbol *target = resolve_function_alias_target(ctx->analyzer, sym, 0);
    if (target && (target->kind == XA_SYM_FUNCTION || target->kind == XA_SYM_METHOD))
        return function_value_target_symbol(target);
    if (sym && sym->is_const && !sym->is_rebindable && symbol_has_function_type(sym) &&
        sym->links.const_initializer) {
        FunctionValueTarget const_target =
            resolve_function_value_expr_target(ctx, sym->links.const_initializer, depth + 1);
        if (function_value_target_is_exact(const_target))
            return const_target;
    }
    FunctionValueTarget alias_target = lookup_function_value_alias_target(ctx, sym);
    if (function_value_target_is_exact(alias_target))
        return alias_target;
    return function_value_target_none();
}

static void maybe_record_function_value_var_initializer(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_VAR_DECL)
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->initializer || decl->symbol_id == 0)
        return;
    XaSymbol *sym = lookup_symbol_by_id(ctx, decl->symbol_id);
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    FunctionValueTarget target = resolve_function_value_expr_target(ctx, decl->initializer, 0);
    if (function_value_target_is_exact(target))
        set_function_value_alias_target(ctx, sym, target);
}

static void record_function_value_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    if (symbol_id == 0)
        return;
    track_function_value_alias_mutation(ctx, sym);
    FunctionValueTarget target = function_value_target_none();
    if (ctx->function_value_control_depth == 0)
        target = resolve_function_value_expr_target(ctx, assign->value, 0);

    invalidate_function_value_alias_target(ctx, symbol_id);

    if (ctx->function_value_control_depth != 0 || !function_value_target_is_exact(target))
        return;
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    set_function_value_alias_target(ctx, sym, target);
}

/* Resolve a call target exactly when possible.  Unknown function values keep
 * their variable symbol so the dynamic-call guard can mark the summary incomplete. */
static FunctionValueTarget resolve_call_target_depth(ErrorSetCtx *ctx, AstNode *callee, int depth) {
    if (!ctx || depth >= 32)
        return function_value_target_none();
    AstNode *source = identity_source(callee);
    const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, source);
    if (sel && sel->target_symbol &&
        (sel->kind == XA_SEL_METHOD || sel->kind == XA_SEL_STATIC_MEMBER ||
         sel->kind == XA_SEL_MODULE_EXPORT)) {
        XaSymbol *selected = sel->target_symbol;
        if (selected->kind == XA_SYM_FUNCTION || selected->kind == XA_SYM_METHOD)
            return function_value_target_symbol(selected);
    }

    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, callee);
    FunctionValueTarget target = resolve_function_value_expr_target(ctx, callee, depth + 1);
    if (function_value_target_is_exact(target))
        return target;
    return function_value_target_symbol(sym);
}

static FunctionValueTarget resolve_call_target(ErrorSetCtx *ctx, AstNode *callee) {
    return resolve_call_target_depth(ctx, callee, 0);
}

static bool is_dynamic_function_call_target(XaAnalyzer *analyzer, AstNode *callee,
                                            XaSymbol *resolved_sym) {
    if (!analyzer || !callee)
        return false;
    if (resolved_sym &&
        (resolved_sym->kind == XA_SYM_FUNCTION || resolved_sym->kind == XA_SYM_METHOD))
        return false;
    XrType *callee_type = xa_analyzer_get_node_type(analyzer, callee);
    if (callee_type && XR_TYPE_IS_FUNCTION(callee_type))
        return true;
    /* The expression side table can be absent on a control-flow merge even
     * though the resolved variable/parameter retains its function type. Do not
     * turn that cache miss into a false NO_THROW result: a non-declaration
     * function symbol without an exact target is dynamic and therefore
     * incomplete (fail-closed). */
    return resolved_sym && symbol_has_function_type(resolved_sym);
}

static bool es_walk_function_expr_body(ErrorSetCtx *ctx, AstNode *function_expr) {
    if (!ctx || !function_expr || function_expr->type != AST_FUNCTION_EXPR)
        return false;
    FunctionDeclNode *fn = &function_expr->as.function_expr;
    if (!fn->body)
        return true;

    /* Exact function-value targets are expanded into the caller's effect
     * summary. A recursive (or mutually recursive) lambda therefore reaches
     * the same expression again while its body is already being visited. The
     * active body already contributes every direct effect in that recursive
     * component, so stop at the back-edge instead of recursively re-expanding
     * it forever. */
    for (int i = 0; i < ctx->active_function_expr_count; i++) {
        if (ctx->active_function_exprs[i] == function_expr)
            return true;
    }
    if (ctx->active_function_expr_count >= ERROR_SET_FUNCTION_EXPR_WALK_MAX) {
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_ANALYSIS_LIMIT);
        return true;
    }
    ErrorSetAliasSnapshot *snapshot =
        (ErrorSetAliasSnapshot *) xr_calloc(1, sizeof(ErrorSetAliasSnapshot));
    if (!snapshot) {
        xa_effect_summary_mark_incomplete(ctx->current_summary,
                                          XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
        return true;
    }
    ctx->active_function_exprs[ctx->active_function_expr_count++] = function_expr;

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, function_expr);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;
    capture_function_value_alias_state(ctx, &snapshot->function_values);
    apply_function_expr_capture(ctx, function_expr);
    const char *saved_catch_var = ctx->current_catch_var;
    uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
    capture_catch_alias_state(ctx, &snapshot->catch_aliases);
    XaEffectSummary *saved_caught = ctx->current_caught;
    int saved_catch_alias_control_depth = ctx->current_catch_alias_control_depth;
    apply_function_expr_catch_capture(ctx, function_expr);
    CoroBoundaryState saved_coro_boundary;
    enter_callee_body_coro_boundary(ctx, &saved_coro_boundary);
    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    es_walk_block(ctx, fn->body);
    leave_callee_body_coro_boundary(ctx, &saved_coro_boundary);
    ctx->current_catch_var = saved_catch_var;
    ctx->current_catch_symbol_id = saved_catch_symbol_id;
    restore_catch_alias_state(ctx, &snapshot->catch_aliases);
    ctx->current_caught = saved_caught;
    ctx->current_catch_alias_control_depth = saved_catch_alias_control_depth;
    restore_function_value_alias_state(ctx, &snapshot->function_values);
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    ctx->current_func = saved_func;
    ctx->analyzer->current_scope = saved_scope;
    ctx->active_function_expr_count--;
    xr_free(snapshot);
    return true;
}

static bool es_walk_immediate_function_expr_call(ErrorSetCtx *ctx, AstNode *callee) {
    AstNode *source = identity_source(callee);
    if (!source || source->type != AST_FUNCTION_EXPR)
        return false;
    return es_walk_function_expr_body(ctx, source);
}

static int function_like_param_count(AstNode *node) {
    if (!node)
        return 0;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.param_count;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.param_count;
    if (node->type == AST_FUNCTION_EXPR)
        return node->as.function_expr.param_count;
    return 0;
}

static XaSymbol *function_like_param_symbol(ErrorSetCtx *ctx, AstNode *node, XaScope *fn_scope,
                                            int index) {
    if (!ctx || !node || index < 0)
        return NULL;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        FunctionDeclNode *fn = &node->as.function_decl;
        if (!fn->params || index >= fn->param_count || !fn->params[index] ||
            fn->params[index]->symbol_id == 0)
            return NULL;
        return lookup_symbol_by_id(ctx, fn->params[index]->symbol_id);
    }
    if (node->type == AST_METHOD_DECL) {
        MethodDeclNode *md = &node->as.method_decl;
        const char *name = (md->params && index < md->param_count && md->params[index])
                               ? md->params[index]->name
                               : NULL;
        return name && fn_scope ? xa_scope_lookup_local(fn_scope, name) : NULL;
    }
    return NULL;
}

static void clear_function_value_param_aliases(ErrorSetCtx *ctx, AstNode *fn_node) {
    if (!ctx || !fn_node)
        return;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);
    int param_count = function_like_param_count(fn_node);
    for (int i = 0; i < param_count; i++) {
        XaSymbol *param_sym = function_like_param_symbol(ctx, fn_node, fn_scope, i);
        if (param_sym && symbol_has_function_type(param_sym))
            invalidate_function_value_alias_target(ctx, param_sym->id);
    }
}

static void apply_specialized_function_param_targets(ErrorSetCtx *ctx, AstNode *fn_node,
                                                     XaSymbol *func_sym) {
    if (!ctx || !fn_node || !is_mono_specialized_function_symbol(func_sym))
        return;
    for (int i = 0; i < ctx->specialized_param_target_count; i++) {
        SpecializedParamTargetEntry *entry = &ctx->specialized_param_targets[i];
        if (entry->function_id != func_sym->id)
            continue;
        XaSymbol *param_sym = lookup_symbol_by_id(ctx, entry->param_id);
        if (!param_sym || !symbol_has_function_type(param_sym)) {
            continue;
        }
        invalidate_function_value_alias_target(ctx, param_sym->id);
        if (!entry->unknown && function_value_target_is_exact(entry->target))
            set_function_value_alias_target(ctx, param_sym, entry->target);
    }
}

static void record_specialized_function_call_targets(ErrorSetCtx *ctx, XaSymbol *callee_sym,
                                                     const CallExprNode *call) {
    if (!ctx || !call || !is_mono_specialized_function_symbol(callee_sym))
        return;
    AstNode *fn_node = callee_sym->links.function_decl_node;
    if (!fn_node)
        return;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);
    int param_count = function_like_param_count(fn_node);
    int n = param_count < call->arg_count ? param_count : call->arg_count;
    for (int i = 0; i < n; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        XaSymbol *param_sym = function_like_param_symbol(ctx, fn_node, fn_scope, i);
        if (!arg || !param_sym || !symbol_has_function_type(param_sym))
            continue;
        FunctionValueTarget arg_target = resolve_function_value_expr_target(ctx, arg, 0);
        if (function_value_target_is_exact(arg_target)) {
            record_specialized_param_target(ctx, callee_sym, param_sym, arg_target, false);
        } else if (expr_has_function_type(ctx, arg)) {
            record_specialized_param_target(ctx, callee_sym, param_sym,
                                            function_value_target_none(), true);
        }
    }
}

static bool es_walk_callsite_function_decl_body(ErrorSetCtx *ctx, XaSymbol *callee_sym,
                                                const CallExprNode *call) {
    if (!ctx || !callee_sym || !call ||
        (callee_sym->kind != XA_SYM_FUNCTION && callee_sym->kind != XA_SYM_METHOD) ||
        ctx->callsite_inline_depth >= 8)
        return false;
    AstNode *fn_node = callee_sym->links.function_decl_node;
    if (!fn_node || (fn_node->type != AST_FUNCTION_DECL && fn_node->type != AST_METHOD_DECL))
        return false;
    AstNode *body = function_like_body(fn_node);
    int param_count = function_like_param_count(fn_node);
    if (!body || param_count <= 0 || call->arg_count <= 0)
        return false;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);

    FunctionValueAliasState *saved_alias_state =
        (FunctionValueAliasState *) xr_calloc(1, sizeof(FunctionValueAliasState));
    if (!saved_alias_state) {
        xa_effect_summary_mark_incomplete(ctx->current_summary,
                                          XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
        return true;
    }
    capture_function_value_alias_state(ctx, saved_alias_state);

    int bound_count = 0;
    int n = param_count < call->arg_count ? param_count : call->arg_count;
    for (int i = 0; i < n; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg)
            continue;
        XaSymbol *param_sym = function_like_param_symbol(ctx, fn_node, fn_scope, i);
        if (!param_sym || !symbol_has_function_type(param_sym))
            continue;
        FunctionValueTarget arg_target = resolve_function_value_expr_target(ctx, arg, 0);
        if (!function_value_target_is_exact(arg_target))
            continue;
        set_function_value_alias_target(ctx, param_sym, arg_target);
        bound_count++;
    }

    if (bound_count == 0) {
        restore_function_value_alias_state(ctx, saved_alias_state);
        xr_free(saved_alias_state);
        return false;
    }

    XaScope *saved_scope = ctx->analyzer->current_scope;
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;

    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    CoroBoundaryState saved_coro_boundary;
    enter_callee_body_coro_boundary(ctx, &saved_coro_boundary);
    ctx->callsite_inline_depth++;
    es_walk_block(ctx, body);
    ctx->callsite_inline_depth--;
    leave_callee_body_coro_boundary(ctx, &saved_coro_boundary);

    restore_function_value_alias_state(ctx, saved_alias_state);
    xr_free(saved_alias_state);
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    ctx->current_func = saved_func;
    ctx->analyzer->current_scope = saved_scope;
    return true;
}

static bool is_current_caught_ref(ErrorSetCtx *ctx, AstNode *expr) {
    expr = identity_source(expr);
    if (!ctx || !expr || !ctx->current_caught)
        return false;

    if (expr->type == AST_INDEX_GET) {
        uint32_t container_id = 0;
        const char *container_name = NULL;
        int64_t index = -1;
        uint32_t index_symbol_id = 0;
        const char *index_symbol_name = NULL;
        const char *index_string = NULL;
        if (!variable_ref_symbol(ctx, expr->as.index_get.array, &container_id, &container_name) ||
            !catch_aggregate_index_key(ctx, expr->as.index_get.index, &index, &index_symbol_id,
                                       &index_symbol_name, &index_string))
            return false;
        CatchAggregateAlias alias = {.container_id = container_id,
                                     .container_name = container_name,
                                     .kind = CATCH_AGGREGATE_INDEX,
                                     .index = index,
                                     .index_symbol_id = index_symbol_id,
                                     .index_symbol_name = index_symbol_name,
                                     .index_string = index_string,
                                     .field_name = NULL};
        return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
    }

    if (expr->type == AST_MEMBER_ACCESS) {
        uint32_t container_id = 0;
        const char *container_name = NULL;
        if (!variable_ref_symbol(ctx, expr->as.member_access.object, &container_id,
                                 &container_name) ||
            !expr->as.member_access.name)
            return false;
        CatchAggregateAlias alias = {.container_id = container_id,
                                     .container_name = container_name,
                                     .kind = CATCH_AGGREGATE_FIELD,
                                     .index = -1,
                                     .field_name = expr->as.member_access.name};
        return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
    }

    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;

    uint32_t symbol_id = expr->as.variable.symbol_id;
    if (symbol_id != 0) {
        if (ctx->current_catch_binding_is_caught && ctx->current_catch_symbol_id != 0 &&
            symbol_id == ctx->current_catch_symbol_id)
            return true;
        for (int i = 0; i < ctx->current_catch_alias_count; i++) {
            if (ctx->current_catch_alias_ids[i] != 0 &&
                symbol_id == ctx->current_catch_alias_ids[i])
                return true;
        }
        return ctx->current_catch_binding_is_caught && ctx->current_catch_var &&
               strcmp(expr->as.variable.name, ctx->current_catch_var) == 0;
    }

    if (ctx->current_catch_binding_is_caught && ctx->current_catch_var &&
        strcmp(expr->as.variable.name, ctx->current_catch_var) == 0)
        return true;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        const char *name = ctx->current_catch_alias_names[i];
        if (name && strcmp(expr->as.variable.name, name) == 0)
            return true;
    }
    return false;
}

static bool catch_symbol_matches(uint32_t lhs_id, const char *lhs_name, uint32_t rhs_id,
                                 const char *rhs_name) {
    if (lhs_id != 0 && rhs_id != 0)
        return lhs_id == rhs_id;
    return lhs_name && rhs_name && strcmp(lhs_name, rhs_name) == 0;
}

static bool current_catch_target_matches(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx)
        return false;
    if (symbol_id != 0 && ctx->current_catch_symbol_id != 0 &&
        symbol_id == ctx->current_catch_symbol_id)
        return true;
    return name && ctx->current_catch_var && strcmp(name, ctx->current_catch_var) == 0;
}

static int current_catch_alias_index(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx)
        return -1;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        if (catch_symbol_matches(symbol_id, name, ctx->current_catch_alias_ids[i],
                                 ctx->current_catch_alias_names[i]))
            return i;
    }
    return -1;
}

static void capture_catch_alias_state(ErrorSetCtx *ctx, CatchAliasState *state) {
    if (!ctx || !state)
        return;
    state->binding_is_caught = ctx->current_catch_binding_is_caught;
    state->alias_count = ctx->current_catch_alias_count;
    if (state->alias_count > 64)
        state->alias_count = 64;
    for (int i = 0; i < state->alias_count; i++) {
        state->alias_ids[i] = ctx->current_catch_alias_ids[i];
        state->alias_names[i] = ctx->current_catch_alias_names[i];
    }
    state->aggregate_alias_count = ctx->current_catch_aggregate_alias_count;
    if (state->aggregate_alias_count > 64)
        state->aggregate_alias_count = 64;
    for (int i = 0; i < state->aggregate_alias_count; i++)
        state->aggregate_aliases[i] = ctx->current_catch_aggregate_aliases[i];
}

static void restore_catch_alias_state(ErrorSetCtx *ctx, const CatchAliasState *state) {
    if (!ctx || !state)
        return;
    ctx->current_catch_binding_is_caught = state->binding_is_caught;
    ctx->current_catch_alias_count = state->alias_count;
    if (ctx->current_catch_alias_count > 64)
        ctx->current_catch_alias_count = 64;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        ctx->current_catch_alias_ids[i] = state->alias_ids[i];
        ctx->current_catch_alias_names[i] = state->alias_names[i];
    }
    ctx->current_catch_aggregate_alias_count = state->aggregate_alias_count;
    if (ctx->current_catch_aggregate_alias_count > 64)
        ctx->current_catch_aggregate_alias_count = 64;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count; i++)
        ctx->current_catch_aggregate_aliases[i] = state->aggregate_aliases[i];
}

static bool catch_alias_state_has(const CatchAliasState *state, uint32_t symbol_id,
                                  const char *name) {
    if (!state)
        return false;
    for (int i = 0; i < state->alias_count; i++) {
        if (catch_symbol_matches(symbol_id, name, state->alias_ids[i], state->alias_names[i]))
            return true;
    }
    return false;
}

static bool catch_aggregate_alias_matches(const CatchAggregateAlias *a,
                                          const CatchAggregateAlias *b) {
    if (!a || !b || a->kind != b->kind ||
        !catch_symbol_matches(a->container_id, a->container_name, b->container_id,
                              b->container_name))
        return false;
    if (a->kind == CATCH_AGGREGATE_EMPTY || a->kind == CATCH_AGGREGATE_SINGLETON ||
        a->kind == CATCH_AGGREGATE_ELEMENT || a->kind == CATCH_AGGREGATE_KEY_ELEMENT ||
        a->kind == CATCH_AGGREGATE_ENTRY_KEY || a->kind == CATCH_AGGREGATE_ENTRY_VALUE)
        return true;
    if (a->kind == CATCH_AGGREGATE_KNOWN_COUNT)
        return a->index == b->index;
    if (a->kind == CATCH_AGGREGATE_INDEX || a->kind == CATCH_AGGREGATE_KEY_INDEX ||
        a->kind == CATCH_AGGREGATE_PRESENT_INDEX) {
        bool a_string = a->index_string != NULL;
        bool b_string = b->index_string != NULL;
        if (a_string || b_string)
            return a_string && b_string && names_equal(a->index_string, b->index_string);
        bool a_symbolic = a->index_symbol_id != 0 || a->index_symbol_name != NULL;
        bool b_symbolic = b->index_symbol_id != 0 || b->index_symbol_name != NULL;
        if (a_symbolic || b_symbolic)
            return a_symbolic && b_symbolic &&
                   catch_symbol_matches(a->index_symbol_id, a->index_symbol_name,
                                        b->index_symbol_id, b->index_symbol_name);
        return a->index == b->index;
    }
    return names_equal(a->field_name, b->field_name);
}

static bool catch_alias_state_has_aggregate(const CatchAliasState *state,
                                            const CatchAggregateAlias *alias) {
    if (!state || !alias)
        return false;
    for (int i = 0; i < state->aggregate_alias_count; i++) {
        if (catch_aggregate_alias_matches(&state->aggregate_aliases[i], alias))
            return true;
    }
    return false;
}

static void merge_catch_alias_intersection_states(ErrorSetCtx *ctx, const CatchAliasState *left,
                                                  const CatchAliasState *right) {
    if (!ctx || !left || !right)
        return;
    ctx->current_catch_binding_is_caught = left->binding_is_caught && right->binding_is_caught;

    uint32_t merged_ids[64];
    const char *merged_names[64];
    int merged_count = 0;
    for (int i = 0; i < left->alias_count && merged_count < 64; i++) {
        uint32_t id = left->alias_ids[i];
        const char *name = left->alias_names[i];
        if (!catch_alias_state_has(right, id, name))
            continue;
        merged_ids[merged_count] = id;
        merged_names[merged_count] = name;
        merged_count++;
    }

    ctx->current_catch_alias_count = merged_count;
    for (int i = 0; i < merged_count; i++) {
        ctx->current_catch_alias_ids[i] = merged_ids[i];
        ctx->current_catch_alias_names[i] = merged_names[i];
    }

    CatchAggregateAlias merged_aggregates[64];
    int merged_aggregate_count = 0;
    for (int i = 0; i < left->aggregate_alias_count && merged_aggregate_count < 64; i++) {
        const CatchAggregateAlias *alias = &left->aggregate_aliases[i];
        if (!catch_alias_state_has_aggregate(right, alias))
            continue;
        merged_aggregates[merged_aggregate_count++] = *alias;
    }
    ctx->current_catch_aggregate_alias_count = merged_aggregate_count;
    for (int i = 0; i < merged_aggregate_count; i++)
        ctx->current_catch_aggregate_aliases[i] = merged_aggregates[i];
}

static void merge_catch_alias_path_states(ErrorSetCtx *ctx, const CatchAliasState **path_states,
                                          int path_count) {
    if (!ctx || !path_states || path_count <= 0 || !path_states[0])
        return;
    restore_catch_alias_state(ctx, path_states[0]);
    for (int i = 1; i < path_count; i++) {
        if (!path_states[i])
            continue;
        CatchAliasState current;
        capture_catch_alias_state(ctx, &current);
        merge_catch_alias_intersection_states(ctx, &current, path_states[i]);
    }
}

static void remove_current_catch_alias(ErrorSetCtx *ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->current_catch_alias_count)
        return;
    for (int i = index; i + 1 < ctx->current_catch_alias_count; i++) {
        ctx->current_catch_alias_ids[i] = ctx->current_catch_alias_ids[i + 1];
        ctx->current_catch_alias_names[i] = ctx->current_catch_alias_names[i + 1];
    }
    ctx->current_catch_alias_count--;
}

static void add_current_catch_alias(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx || symbol_id == 0 || !name || ctx->current_catch_alias_count >= 64)
        return;
    if (current_catch_alias_index(ctx, symbol_id, name) >= 0)
        return;
    int slot = ctx->current_catch_alias_count++;
    ctx->current_catch_alias_ids[slot] = symbol_id;
    ctx->current_catch_alias_names[slot] = name;
}

static int current_catch_aggregate_alias_index(ErrorSetCtx *ctx, const CatchAggregateAlias *alias) {
    if (!ctx || !alias)
        return -1;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count; i++) {
        if (catch_aggregate_alias_matches(&ctx->current_catch_aggregate_aliases[i], alias))
            return i;
    }
    return -1;
}

static void remove_current_catch_aggregate_alias(ErrorSetCtx *ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->current_catch_aggregate_alias_count)
        return;
    for (int i = index; i + 1 < ctx->current_catch_aggregate_alias_count; i++)
        ctx->current_catch_aggregate_aliases[i] = ctx->current_catch_aggregate_aliases[i + 1];
    ctx->current_catch_aggregate_alias_count--;
}

static void remove_current_catch_aggregate_aliases_for_container(ErrorSetCtx *ctx,
                                                                 uint32_t symbol_id,
                                                                 const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return;
    for (int i = ctx->current_catch_aggregate_alias_count - 1; i >= 0; i--) {
        CatchAggregateAlias *alias = &ctx->current_catch_aggregate_aliases[i];
        if (catch_symbol_matches(symbol_id, name, alias->container_id, alias->container_name))
            remove_current_catch_aggregate_alias(ctx, i);
    }
}

static void remove_current_catch_aggregate_alias_for_index_kind(
    ErrorSetCtx *ctx, uint32_t symbol_id, const char *name, CatchAggregateAliasKind kind,
    int64_t index, uint32_t index_symbol_id, const char *index_symbol_name,
    const char *index_string) {
    if (!ctx || (symbol_id == 0 && !name))
        return;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = kind,
                                 .index = index,
                                 .index_symbol_id = index_symbol_id,
                                 .index_symbol_name = index_symbol_name,
                                 .index_string = index_string,
                                 .field_name = NULL};
    int alias_index = current_catch_aggregate_alias_index(ctx, &alias);
    if (alias_index >= 0)
        remove_current_catch_aggregate_alias(ctx, alias_index);
}

static void remove_current_catch_aggregate_alias_for_index(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                           const char *name, int64_t index,
                                                           uint32_t index_symbol_id,
                                                           const char *index_symbol_name,
                                                           const char *index_string) {
    remove_current_catch_aggregate_alias_for_index_kind(ctx, symbol_id, name, CATCH_AGGREGATE_INDEX,
                                                        index, index_symbol_id, index_symbol_name,
                                                        index_string);
}

static void remove_current_catch_aggregate_alias_for_element(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                             const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_ELEMENT,
                                 .index = -1};
    int alias_index = current_catch_aggregate_alias_index(ctx, &alias);
    if (alias_index >= 0)
        remove_current_catch_aggregate_alias(ctx, alias_index);
}

static void remove_current_catch_aggregate_aliases_for_kind(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                            const char *name,
                                                            CatchAggregateAliasKind kind) {
    if (!ctx || (symbol_id == 0 && !name))
        return;
    for (int i = ctx->current_catch_aggregate_alias_count - 1; i >= 0; i--) {
        CatchAggregateAlias *alias = &ctx->current_catch_aggregate_aliases[i];
        if (alias->kind == kind &&
            catch_symbol_matches(symbol_id, name, alias->container_id, alias->container_name))
            remove_current_catch_aggregate_alias(ctx, i);
    }
}

static void add_current_catch_aggregate_alias(ErrorSetCtx *ctx, uint32_t symbol_id,
                                              const char *name, CatchAggregateAliasKind kind,
                                              int64_t index, uint32_t index_symbol_id,
                                              const char *index_symbol_name,
                                              const char *index_string, const char *field_name) {
    if (!ctx || (symbol_id == 0 && !name) || ctx->current_catch_aggregate_alias_count >= 64)
        return;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = kind,
                                 .index = index,
                                 .index_symbol_id = index_symbol_id,
                                 .index_symbol_name = index_symbol_name,
                                 .index_string = index_string,
                                 .field_name = field_name};
    if (current_catch_aggregate_alias_index(ctx, &alias) >= 0)
        return;
    ctx->current_catch_aggregate_aliases[ctx->current_catch_aggregate_alias_count++] = alias;
}

static bool current_catch_has_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                  const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count; i++) {
        CatchAggregateAlias *alias = &ctx->current_catch_aggregate_aliases[i];
        if (catch_symbol_matches(symbol_id, name, alias->container_id, alias->container_name))
            return true;
    }
    return false;
}

static bool current_catch_has_element_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                          const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_ELEMENT,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_singleton_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                            const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_SINGLETON,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_empty_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                        const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_EMPTY,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_index_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                        const char *name, int64_t index,
                                                        uint32_t index_symbol_id,
                                                        const char *index_symbol_name,
                                                        const char *index_string) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_INDEX,
                                 .index = index,
                                 .index_symbol_id = index_symbol_id,
                                 .index_symbol_name = index_symbol_name,
                                 .index_string = index_string,
                                 .field_name = NULL};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_present_index_aggregate_container(
    ErrorSetCtx *ctx, uint32_t symbol_id, const char *name, int64_t index, uint32_t index_symbol_id,
    const char *index_symbol_name, const char *index_string) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_PRESENT_INDEX,
                                 .index = index,
                                 .index_symbol_id = index_symbol_id,
                                 .index_symbol_name = index_symbol_name,
                                 .index_string = index_string,
                                 .field_name = NULL};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_known_aggregate_count(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                const char *name, int64_t *count) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count; i++) {
        CatchAggregateAlias *alias = &ctx->current_catch_aggregate_aliases[i];
        if (alias->kind == CATCH_AGGREGATE_KNOWN_COUNT &&
            catch_symbol_matches(symbol_id, name, alias->container_id, alias->container_name)) {
            if (count)
                *count = alias->index;
            return true;
        }
    }
    return false;
}

static int current_catch_aggregate_alias_count_for_kind(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                        const char *name,
                                                        CatchAggregateAliasKind kind) {
    if (!ctx || (symbol_id == 0 && !name))
        return 0;
    int count = 0;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count; i++) {
        CatchAggregateAlias *alias = &ctx->current_catch_aggregate_aliases[i];
        if (alias->kind == kind &&
            catch_symbol_matches(symbol_id, name, alias->container_id, alias->container_name))
            count++;
    }
    return count;
}

static bool current_catch_has_key_element_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                              const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_KEY_ELEMENT,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_entry_key_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                            const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_ENTRY_KEY,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool current_catch_has_entry_value_aggregate_container(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                              const char *name) {
    if (!ctx || (symbol_id == 0 && !name))
        return false;
    CatchAggregateAlias alias = {.container_id = symbol_id,
                                 .container_name = name,
                                 .kind = CATCH_AGGREGATE_ENTRY_VALUE,
                                 .index = -1};
    return current_catch_aggregate_alias_index(ctx, &alias) >= 0;
}

static bool catch_collection_view_aliases_from_call(ErrorSetCtx *ctx, AstNode *expr,
                                                    CatchCollectionViewAliases *aliases) {
    if (!aliases)
        return false;
    *aliases = (CatchCollectionViewAliases) {0};
    expr = identity_source(expr);
    if (!ctx || !expr || expr->type != AST_CALL_EXPR)
        return false;

    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->arg_count != 0)
        return false;
    AstNode *callee = identity_source(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return false;

    XrType *receiver_type = xa_analyzer_get_node_type(ctx->analyzer, ma->object);
    if (!receiver_type || (!XR_TYPE_IS_MAP(receiver_type) && !XR_TYPE_IS_SET(receiver_type)))
        return false;
    uint32_t source_id = 0;
    const char *source_name = NULL;
    if (!variable_ref_symbol(ctx, ma->object, &source_id, &source_name))
        return false;

    if (XR_TYPE_IS_MAP(receiver_type)) {
        if (strcmp(ma->name, "values") == 0) {
            aliases->element =
                current_catch_has_element_aggregate_container(ctx, source_id, source_name);
            return true;
        }
        if (strcmp(ma->name, "keys") == 0) {
            aliases->key_element =
                current_catch_has_key_element_aggregate_container(ctx, source_id, source_name);
            return true;
        }
        if (strcmp(ma->name, "entries") == 0) {
            aliases->entry_key =
                current_catch_has_key_element_aggregate_container(ctx, source_id, source_name);
            aliases->entry_value =
                current_catch_has_element_aggregate_container(ctx, source_id, source_name);
            return true;
        }
        return false;
    }

    if (strcmp(ma->name, "values") == 0) {
        aliases->empty = current_catch_has_empty_aggregate_container(ctx, source_id, source_name);
        aliases->element =
            current_catch_has_element_aggregate_container(ctx, source_id, source_name);
        return true;
    }
    return false;
}

static bool literal_i64_index(AstNode *expr, int64_t *out) {
    expr = identity_source(expr);
    if (!expr || expr->type != AST_LITERAL_INT || expr->as.literal.int_overflows_i64 ||
        expr->as.literal.raw_value.int_val < 0)
        return false;
    if (out)
        *out = expr->as.literal.raw_value.int_val;
    return true;
}

static bool catch_aggregate_index_key(ErrorSetCtx *ctx, AstNode *expr, int64_t *literal_index,
                                      uint32_t *symbol_id, const char **symbol_name,
                                      const char **string_key) {
    if (literal_index)
        *literal_index = -1;
    if (symbol_id)
        *symbol_id = 0;
    if (symbol_name)
        *symbol_name = NULL;
    if (string_key)
        *string_key = NULL;

    if (literal_i64_index(expr, literal_index))
        return true;

    expr = identity_source(expr);
    if (expr && expr->type == AST_LITERAL_STRING) {
        const char *value = expr->as.literal.raw_value.string_val;
        if (string_key)
            *string_key = value;
        return value != NULL;
    }
    const XaSelection *sel =
        (ctx && ctx->analyzer) ? xa_analyzer_get_selection(ctx->analyzer, expr) : NULL;
    if (sel && sel->kind == XA_SEL_ENUM_MEMBER) {
        const char *value = NULL;
        if (expr->type == AST_ENUM_ACCESS)
            value = expr->as.enum_access.member_name;
        else if (expr->type == AST_MEMBER_ACCESS)
            value = expr->as.member_access.name;
        if (string_key)
            *string_key = value;
        return value != NULL;
    }
    if (expr && expr->type == AST_ENUM_ACCESS) {
        const char *value = expr->as.enum_access.member_name;
        if (string_key)
            *string_key = value;
        return value != NULL;
    }
    if (!ctx || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
    if (is_current_caught_ref(ctx, expr)) {
        XaSymbol *caught_sym = lookup_variable_symbol(ctx->analyzer, expr);
        if (symbol_id)
            *symbol_id = caught_sym ? caught_sym->id : expr->as.variable.symbol_id;
        if (symbol_name)
            *symbol_name =
                caught_sym && caught_sym->name ? caught_sym->name : expr->as.variable.name;
        return (symbol_id && *symbol_id != 0) || (symbol_name && *symbol_name != NULL);
    }
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, expr);
    if (!sym || (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER))
        return false;

    bool stable = false;
    if (sym->kind == XA_SYM_PARAMETER)
        stable = sym->links.assign_count == 0;
    else
        stable = sym->is_const || sym->links.assign_count == 1;
    if (!stable)
        return false;

    if (symbol_id)
        *symbol_id = sym->id;
    if (symbol_name)
        *symbol_name = sym->name;
    return sym->id != 0 || sym->name != NULL;
}

static bool current_caught_may_match_enum_member(ErrorSetCtx *ctx, AstNode *expr) {
    expr = identity_source(expr);
    if (!ctx || !ctx->analyzer || !ctx->current_caught || !expr)
        return false;
    if (ctx->current_caught->completeness == XA_EFFECT_INCOMPLETE)
        return true;

    const char *member_name = NULL;
    if (expr->type == AST_ENUM_ACCESS)
        member_name = expr->as.enum_access.member_name;
    else if (expr->type == AST_MEMBER_ACCESS)
        member_name = expr->as.member_access.name;
    else
        return false;
    if (!member_name)
        return true;

    const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, expr);
    XrType *enum_type = sel && sel->kind == XA_SEL_ENUM_MEMBER ? sel->result_type : NULL;
    if ((!enum_type || !XR_TYPE_IS_ENUM(enum_type)) && sel && sel->target_symbol)
        enum_type = sel->target_symbol->links.type;
    if ((!enum_type || !XR_TYPE_IS_ENUM(enum_type)))
        enum_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    const char *enum_name = NULL;
    if (expr->type == AST_ENUM_ACCESS)
        enum_name = expr->as.enum_access.enum_name;
    else if (expr->type == AST_MEMBER_ACCESS && expr->as.member_access.object &&
             expr->as.member_access.object->type == AST_VARIABLE)
        enum_name = expr->as.member_access.object->as.variable.name;
    if ((!enum_type || !XR_TYPE_IS_ENUM(enum_type)) && enum_name) {
        XaSymbol *sym = xa_analyzer_lookup(ctx->analyzer, enum_name);
        if (!sym)
            sym =
                xa_analyzer_lookup_in_scope(ctx->analyzer, enum_name, ctx->analyzer->global_scope);
        if (!sym)
            sym = xa_analyzer_lookup_deep(ctx->analyzer, enum_name);
        if (sym && sym->kind == XA_SYM_ENUM)
            enum_type = sym->links.type;
    }
    if (!enum_type || !XR_TYPE_IS_ENUM(enum_type))
        return true;
    XaErrorTypeId type_id = xa_effect_db_register_error_enum(ctx->analyzer->effect_db, enum_type);
    if (type_id == XA_ERROR_TYPE_NONE)
        return true;
    int variant = sel && sel->kind == XA_SEL_ENUM_MEMBER ? sel->field_index : -1;
    if (variant < 0)
        variant = enum_variant_index_for_name(ctx->analyzer, enum_type, member_name);
    if (variant < 0)
        return true;

    for (uint32_t i = 0; i < ctx->current_caught->escaping.count; i++) {
        const XaErrorTypeSet *type_set = &ctx->current_caught->escaping.types[i];
        if (type_set->type_id != type_id)
            continue;
        return type_set->all_variants || xa_bitset_test(&type_set->variants, (uint32_t) variant);
    }
    return false;
}

static bool variable_ref_symbol(ErrorSetCtx *ctx, AstNode *expr, uint32_t *symbol_id,
                                const char **name) {
    expr = identity_source(expr);
    if (!ctx || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, expr);
    if (symbol_id)
        *symbol_id = sym ? sym->id : expr->as.variable.symbol_id;
    if (name)
        *name = expr->as.variable.name;
    return true;
}

static bool expr_references_catch_aggregate_container(ErrorSetCtx *ctx, AstNode *expr,
                                                      uint32_t *symbol_id, const char **name) {
    uint32_t id = 0;
    const char *container_name = NULL;
    if (!variable_ref_symbol(ctx, expr, &id, &container_name))
        return false;
    if (!current_catch_has_aggregate_container(ctx, id, container_name))
        return false;
    if (symbol_id)
        *symbol_id = id;
    if (name)
        *name = container_name;
    return true;
}

static void invalidate_catch_aggregate_container_ref(ErrorSetCtx *ctx, AstNode *expr) {
    uint32_t symbol_id = 0;
    const char *name = NULL;
    if (expr_references_catch_aggregate_container(ctx, expr, &symbol_id, &name))
        remove_current_catch_aggregate_aliases_for_container(ctx, symbol_id, name);
}

static bool call_arg_preserves_catch_aggregate_ref(ErrorSetCtx *ctx, const CallExprNode *call,
                                                   int arg_index) {
    if (!ctx || !call || arg_index != 0 || call->arg_count != 1 || !call->callee ||
        !call->arguments || !call->arguments[0])
        return false;
    AstNode *callee = identity_source(call->callee);
    if (!callee || callee->type != AST_VARIABLE || !callee->as.variable.name ||
        strcmp(callee->as.variable.name, "len") != 0)
        return false;
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, callee);
    if (!sym || sym->kind != XA_SYM_FUNCTION || !sym->is_builtin || !sym->name ||
        strcmp(sym->name, "len") != 0)
        return false;
    XrType *arg_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[0]);
    return arg_type && (XR_TYPE_IS_MAP(arg_type) || XR_TYPE_IS_SET(arg_type));
}

static bool catch_alias_declared_in_block(ErrorSetCtx *ctx, uint32_t symbol_id, AstNode *block) {
    if (!ctx || !block || block->type != AST_BLOCK || symbol_id == 0)
        return true;
    XaScope *block_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, block);
    XaSymbol *sym = lookup_symbol_by_id(ctx, symbol_id);
    return !block_scope || !sym || sym->scope == block_scope;
}

static void finish_catch_alias_block(ErrorSetCtx *ctx, AstNode *block, int saved_count,
                                     int saved_aggregate_count) {
    if (!ctx)
        return;
    if (!ctx->current_caught || ctx->current_catch_alias_control_depth != 0) {
        ctx->current_catch_alias_count = saved_count;
        ctx->current_catch_aggregate_alias_count = saved_aggregate_count;
        return;
    }

    uint32_t final_ids[64];
    const char *final_names[64];
    int final_count = 0;
    for (int i = 0; i < ctx->current_catch_alias_count && final_count < 64; i++) {
        uint32_t id = ctx->current_catch_alias_ids[i];
        const char *name = ctx->current_catch_alias_names[i];
        if (catch_alias_declared_in_block(ctx, id, block))
            continue;
        final_ids[final_count] = id;
        final_names[final_count] = name;
        final_count++;
    }
    ctx->current_catch_alias_count = final_count;
    for (int i = 0; i < final_count; i++) {
        ctx->current_catch_alias_ids[i] = final_ids[i];
        ctx->current_catch_alias_names[i] = final_names[i];
    }

    CatchAggregateAlias final_aggregates[64];
    int final_aggregate_count = 0;
    for (int i = 0; i < ctx->current_catch_aggregate_alias_count && final_aggregate_count < 64;
         i++) {
        CatchAggregateAlias alias = ctx->current_catch_aggregate_aliases[i];
        if (catch_alias_declared_in_block(ctx, alias.container_id, block))
            continue;
        final_aggregates[final_aggregate_count++] = alias;
    }
    ctx->current_catch_aggregate_alias_count = final_aggregate_count;
    for (int i = 0; i < final_aggregate_count; i++)
        ctx->current_catch_aggregate_aliases[i] = final_aggregates[i];
}

static void maybe_record_catch_alias(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !ctx->current_caught ||
        (node->type != AST_CONST_DECL && node->type != AST_VAR_DECL))
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->name || !decl->initializer || decl->symbol_id == 0 ||
        ctx->current_catch_alias_count >= 64)
        return;
    if (node->type == AST_VAR_DECL) {
        XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, decl->symbol_id);
        if (!sym || sym->kind != XA_SYM_VARIABLE || sym->links.assign_count != 1)
            return;
    }
    if (!is_current_caught_ref(ctx, decl->initializer))
        return;
    add_current_catch_alias(ctx, decl->symbol_id, decl->name);
}

static void record_catch_aggregate_entries_from_initializer(ErrorSetCtx *ctx, uint32_t symbol_id,
                                                            const char *name,
                                                            AstNode *initializer) {
    initializer = identity_source(initializer);
    if (!ctx || !ctx->current_caught || (symbol_id == 0 && !name) || !initializer)
        return;

    switch (initializer->type) {
        case AST_ARRAY_LITERAL:
            if (initializer->as.array_literal.is_repeat)
                return;
            for (int i = 0; i < initializer->as.array_literal.count; i++) {
                if (is_current_caught_ref(ctx, initializer->as.array_literal.elements[i]))
                    add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_INDEX,
                                                      i, 0, NULL, NULL, NULL);
            }
            break;

        case AST_STRUCT_LITERAL:
            for (int i = 0; i < initializer->as.struct_literal.field_count; i++) {
                const char *field = initializer->as.struct_literal.field_names[i];
                if (field &&
                    is_current_caught_ref(ctx, initializer->as.struct_literal.field_values[i]))
                    add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_FIELD,
                                                      -1, 0, NULL, NULL, field);
            }
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < initializer->as.object_literal.count; i++) {
                AstNode *key = initializer->as.object_literal.keys[i];
                if (!key || key->type != AST_LITERAL_STRING)
                    continue;
                const char *field = key->as.literal.raw_value.string_val;
                if (field && is_current_caught_ref(ctx, initializer->as.object_literal.values[i]))
                    add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_FIELD,
                                                      -1, 0, NULL, NULL, field);
            }
            break;

        case AST_MAP_LITERAL: {
            if (initializer->as.map_literal.count == 0) {
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_EMPTY, -1,
                                                  0, NULL, NULL, NULL);
                break;
            }
            bool all_values_are_caught = true;
            bool all_keys_are_caught = true;
            bool all_keys_have_known_present_identity = true;
            bool tracked_map_entry = false;
            for (int i = 0; i < initializer->as.map_literal.count; i++) {
                bool value_is_caught =
                    is_current_caught_ref(ctx, initializer->as.map_literal.values[i]);
                bool key_is_caught =
                    is_current_caught_ref(ctx, initializer->as.map_literal.keys[i]);
                if (!value_is_caught)
                    all_values_are_caught = false;
                if (!key_is_caught)
                    all_keys_are_caught = false;

                int64_t present_index = -1;
                uint32_t present_index_symbol_id = 0;
                const char *present_index_symbol_name = NULL;
                const char *present_index_string = NULL;
                bool has_present_index = catch_aggregate_index_key(
                    ctx, initializer->as.map_literal.keys[i], &present_index,
                    &present_index_symbol_id, &present_index_symbol_name, &present_index_string);
                if (has_present_index &&
                    ((present_index_symbol_id == 0 && present_index_symbol_name == NULL) ||
                     key_is_caught)) {
                    add_current_catch_aggregate_alias(
                        ctx, symbol_id, name, CATCH_AGGREGATE_PRESENT_INDEX, present_index,
                        present_index_symbol_id, present_index_symbol_name, present_index_string,
                        NULL);
                } else {
                    all_keys_have_known_present_identity = false;
                }
                if (key_is_caught && has_present_index)
                    add_current_catch_aggregate_alias(
                        ctx, symbol_id, name, CATCH_AGGREGATE_KEY_INDEX, present_index,
                        present_index_symbol_id, present_index_symbol_name, present_index_string,
                        NULL);

                int64_t index = -1;
                uint32_t index_symbol_id = 0;
                const char *index_symbol_name = NULL;
                const char *index_string = NULL;
                if (!value_is_caught ||
                    !catch_aggregate_index_key(ctx, initializer->as.map_literal.keys[i], &index,
                                               &index_symbol_id, &index_symbol_name, &index_string))
                    continue;
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_INDEX,
                                                  index, index_symbol_id, index_symbol_name,
                                                  index_string, NULL);
                tracked_map_entry = true;
            }
            if (initializer->as.map_literal.count > 0 && all_values_are_caught)
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_ELEMENT, -1,
                                                  0, NULL, NULL, NULL);
            if (initializer->as.map_literal.count > 0 && all_keys_are_caught)
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_KEY_ELEMENT,
                                                  -1, 0, NULL, NULL, NULL);
            int known_present_count = current_catch_aggregate_alias_count_for_kind(
                ctx, symbol_id, name, CATCH_AGGREGATE_PRESENT_INDEX);
            if (all_keys_have_known_present_identity &&
                known_present_count == initializer->as.map_literal.count)
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_KNOWN_COUNT,
                                                  known_present_count, 0, NULL, NULL, NULL);
            if (initializer->as.map_literal.count == 1 &&
                (tracked_map_entry || all_values_are_caught || all_keys_are_caught))
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_SINGLETON,
                                                  -1, 0, NULL, NULL, NULL);
            break;
        }

        case AST_SET_LITERAL:
            if (initializer->as.set_literal.count <= 0) {
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_EMPTY, -1,
                                                  0, NULL, NULL, NULL);
                break;
            }
            bool all_elements_are_caught = true;
            for (int i = 0; i < initializer->as.set_literal.count; i++) {
                if (!is_current_caught_ref(ctx, initializer->as.set_literal.elements[i])) {
                    all_elements_are_caught = false;
                    break;
                }
            }
            if (!all_elements_are_caught)
                break;
            add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_ELEMENT, -1, 0,
                                              NULL, NULL, NULL);
            break;

        case AST_CALL_EXPR: {
            CatchCollectionViewAliases aliases;
            if (!catch_collection_view_aliases_from_call(ctx, initializer, &aliases))
                break;
            if (aliases.element) {
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_ELEMENT, -1,
                                                  0, NULL, NULL, NULL);
            }
            if (aliases.key_element) {
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_KEY_ELEMENT,
                                                  -1, 0, NULL, NULL, NULL);
            }
            if (aliases.entry_key)
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_ENTRY_KEY,
                                                  -1, 0, NULL, NULL, NULL);
            if (aliases.entry_value)
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_ENTRY_VALUE,
                                                  -1, 0, NULL, NULL, NULL);
            break;
        }

        default:
            break;
    }
}

static void maybe_add_for_in_catch_alias(ErrorSetCtx *ctx, ForInStmtNode *fi) {
    if (!ctx || !fi || !ctx->current_caught)
        return;
    CatchCollectionViewAliases aliases = {0};
    uint32_t collection_id = 0;
    const char *collection_name = NULL;
    bool has_collection_symbol =
        variable_ref_symbol(ctx, fi->collection, &collection_id, &collection_name);
    if (has_collection_symbol) {
        aliases.element =
            current_catch_has_element_aggregate_container(ctx, collection_id, collection_name);
        aliases.key_element =
            current_catch_has_key_element_aggregate_container(ctx, collection_id, collection_name);
        aliases.entry_key =
            current_catch_has_entry_key_aggregate_container(ctx, collection_id, collection_name);
        aliases.entry_value =
            current_catch_has_entry_value_aggregate_container(ctx, collection_id, collection_name);
    } else {
        catch_collection_view_aliases_from_call(ctx, fi->collection, &aliases);
    }
    bool element_is_caught = aliases.element;
    bool key_element_is_caught = aliases.key_element;
    bool entry_key_is_caught = aliases.entry_key;
    bool entry_value_is_caught = aliases.entry_value;
    if (!element_is_caught && !key_element_is_caught && !entry_key_is_caught &&
        !entry_value_is_caught)
        return;

    XrType *collection_type = xa_analyzer_get_node_type(ctx->analyzer, fi->collection);
    if (fi->is_keyvalue) {
        if (!collection_type || !XR_TYPE_IS_MAP(collection_type))
            return;
        if (key_element_is_caught && fi->item_symbol_id != 0 && fi->item_name)
            add_current_catch_alias(ctx, fi->item_symbol_id, fi->item_name);
        if (element_is_caught && fi->value_symbol_id != 0 && fi->value_name)
            add_current_catch_alias(ctx, fi->value_symbol_id, fi->value_name);
        return;
    }

    if (fi->item_symbol_id == 0 || !fi->item_name)
        return;
    if (collection_type && XR_TYPE_IS_MAP(collection_type)) {
        if (key_element_is_caught)
            add_current_catch_alias(ctx, fi->item_symbol_id, fi->item_name);
        return;
    }
    bool added_entry_slot_alias = false;
    if (entry_key_is_caught) {
        add_current_catch_aggregate_alias(ctx, fi->item_symbol_id, fi->item_name,
                                          CATCH_AGGREGATE_INDEX, 0, 0, NULL, NULL, NULL);
        added_entry_slot_alias = true;
    }
    if (entry_value_is_caught) {
        add_current_catch_aggregate_alias(ctx, fi->item_symbol_id, fi->item_name,
                                          CATCH_AGGREGATE_INDEX, 1, 0, NULL, NULL, NULL);
        added_entry_slot_alias = true;
    }
    if (added_entry_slot_alias)
        return;
    add_current_catch_alias(ctx, fi->item_symbol_id, fi->item_name);
}

static bool for_in_collection_is_known_empty_set(ErrorSetCtx *ctx, ForInStmtNode *fi) {
    if (!ctx || !fi || !ctx->current_caught)
        return false;
    XrType *collection_type = xa_analyzer_get_node_type(ctx->analyzer, fi->collection);
    if (collection_type && XR_TYPE_IS_SET(collection_type)) {
        uint32_t collection_id = 0;
        const char *collection_name = NULL;
        return variable_ref_symbol(ctx, fi->collection, &collection_id, &collection_name) &&
               current_catch_has_empty_aggregate_container(ctx, collection_id, collection_name);
    }
    CatchCollectionViewAliases aliases = {0};
    return catch_collection_view_aliases_from_call(ctx, fi->collection, &aliases) && aliases.empty;
}

typedef enum CatchAggregateMemberAction {
    CATCH_AGGREGATE_MEMBER_INVALIDATE,
    CATCH_AGGREGATE_MEMBER_PRESERVE,
    CATCH_AGGREGATE_MEMBER_SET_ADD_CAUGHT,
    CATCH_AGGREGATE_MEMBER_SET_DELETE_CAUGHT,
    CATCH_AGGREGATE_MEMBER_SET_CLEAR,
    CATCH_AGGREGATE_MEMBER_MAP_SET_PRECISE,
    CATCH_AGGREGATE_MEMBER_MAP_DELETE_PRECISE,
    CATCH_AGGREGATE_MEMBER_MAP_CLEAR,
} CatchAggregateMemberAction;

typedef struct CatchAggregateMemberUpdate {
    CatchAggregateMemberAction action;
    uint32_t container_id;
    const char *container_name;
    int64_t index;
    uint32_t index_symbol_id;
    const char *index_symbol_name;
    const char *index_string;
    bool has_precise_index;
    bool index_may_alias_other_slots;
    bool key_is_caught;
    bool key_set_starts_tracked_map;
    bool value_is_caught;
    bool value_set_starts_tracked_map;
    bool map_delete_makes_empty;
    bool has_known_count;
    int64_t known_count;
    bool delete_known_present_index;
} CatchAggregateMemberUpdate;

static CatchAggregateMemberUpdate catch_aggregate_member_update(ErrorSetCtx *ctx, AstNode *callee,
                                                                const CallExprNode *call) {
    CatchAggregateMemberUpdate update = {.action = CATCH_AGGREGATE_MEMBER_INVALIDATE, .index = -1};
    AstNode *source = identity_source(callee);
    if (!ctx || !ctx->current_caught || !source || source->type != AST_MEMBER_ACCESS || !call ||
        (call->arg_count > 0 && !call->arguments))
        return update;

    MemberAccessNode *ma = &source->as.member_access;
    if (!ma->name || !ma->object)
        return update;
    XrType *receiver_type = xa_analyzer_get_node_type(ctx->analyzer, ma->object);
    if (!receiver_type || (!XR_TYPE_IS_SET(receiver_type) && !XR_TYPE_IS_MAP(receiver_type)))
        return update;

    uint32_t id = 0;
    const char *name = NULL;
    if (!variable_ref_symbol(ctx, ma->object, &id, &name))
        return update;
    update.container_id = id;
    update.container_name = name;

    if (XR_TYPE_IS_SET(receiver_type)) {
        if (!current_catch_has_aggregate_container(ctx, id, name))
            return update;
        bool set_is_known_empty = current_catch_has_empty_aggregate_container(ctx, id, name);
        bool set_has_caught_elements = current_catch_has_element_aggregate_container(ctx, id, name);
        bool set_delete_may_remove_caught =
            call->arg_count == 1 && strcmp(ma->name, "delete") == 0 && set_has_caught_elements &&
            current_caught_may_match_enum_member(ctx, call->arguments[0]);
        if (call->arg_count == 0 && strcmp(ma->name, "clear") == 0) {
            update.action = CATCH_AGGREGATE_MEMBER_SET_CLEAR;
            return update;
        }
        if (call->arg_count == 1 && strcmp(ma->name, "delete") == 0 &&
            (set_has_caught_elements || set_is_known_empty) &&
            is_current_caught_ref(ctx, call->arguments[0])) {
            update.action = CATCH_AGGREGATE_MEMBER_SET_DELETE_CAUGHT;
            return update;
        }
        if ((call->arg_count == 1 && strcmp(ma->name, "contains") == 0) ||
            (call->arg_count == 0 && strcmp(ma->name, "values") == 0) ||
            (call->arg_count == 1 && strcmp(ma->name, "delete") == 0 &&
             (set_is_known_empty || (set_has_caught_elements && !set_delete_may_remove_caught)))) {
            update.action = CATCH_AGGREGATE_MEMBER_PRESERVE;
            return update;
        }
        if (call->arg_count == 1 && strcmp(ma->name, "add") == 0 &&
            (set_has_caught_elements || set_is_known_empty) &&
            is_current_caught_ref(ctx, call->arguments[0])) {
            update.action = CATCH_AGGREGATE_MEMBER_SET_ADD_CAUGHT;
            return update;
        }
        return update;
    }

    if (call->arg_count == 2 && strcmp(ma->name, "set") == 0) {
        update.has_precise_index = catch_aggregate_index_key(
            ctx, call->arguments[0], &update.index, &update.index_symbol_id,
            &update.index_symbol_name, &update.index_string);
        update.index_may_alias_other_slots =
            update.has_precise_index &&
            (update.index_symbol_id != 0 || update.index_symbol_name != NULL ||
             current_caught_may_match_enum_member(ctx, call->arguments[0]));
        update.key_is_caught = is_current_caught_ref(ctx, call->arguments[0]);
        update.value_is_caught = is_current_caught_ref(ctx, call->arguments[1]);
        bool map_has_tracked_entries = current_catch_has_aggregate_container(ctx, id, name);
        bool map_is_known_empty = current_catch_has_empty_aggregate_container(ctx, id, name);
        update.key_set_starts_tracked_map = update.key_is_caught && map_is_known_empty;
        update.value_set_starts_tracked_map = update.value_is_caught && map_is_known_empty;
        if (update.key_is_caught || update.value_is_caught || map_has_tracked_entries)
            update.action = CATCH_AGGREGATE_MEMBER_MAP_SET_PRECISE;
        return update;
    }
    if (call->arg_count == 0 && strcmp(ma->name, "clear") == 0) {
        update.action = CATCH_AGGREGATE_MEMBER_MAP_CLEAR;
        return update;
    }
    if (!current_catch_has_aggregate_container(ctx, id, name))
        return update;
    if (call->arg_count == 1 &&
        (strcmp(ma->name, "get") == 0 || strcmp(ma->name, "containsKey") == 0 ||
         strcmp(ma->name, "containsValue") == 0)) {
        update.action = CATCH_AGGREGATE_MEMBER_PRESERVE;
        return update;
    }
    if (call->arg_count == 0 && (strcmp(ma->name, "keys") == 0 || strcmp(ma->name, "values") == 0 ||
                                 strcmp(ma->name, "entries") == 0)) {
        update.action = CATCH_AGGREGATE_MEMBER_PRESERVE;
        return update;
    }
    if (call->arg_count == 1 && strcmp(ma->name, "delete") == 0) {
        update.has_precise_index = catch_aggregate_index_key(
            ctx, call->arguments[0], &update.index, &update.index_symbol_id,
            &update.index_symbol_name, &update.index_string);
        update.index_may_alias_other_slots =
            update.has_precise_index &&
            (update.index_symbol_id != 0 || update.index_symbol_name != NULL ||
             current_caught_may_match_enum_member(ctx, call->arguments[0]));
        update.has_known_count =
            current_catch_known_aggregate_count(ctx, id, name, &update.known_count);
        update.delete_known_present_index = update.has_precise_index &&
                                            !update.index_may_alias_other_slots &&
                                            current_catch_has_present_index_aggregate_container(
                                                ctx, id, name, update.index, update.index_symbol_id,
                                                update.index_symbol_name, update.index_string);
        bool map_is_singleton = current_catch_has_singleton_aggregate_container(ctx, id, name);
        if (update.has_known_count && update.known_count == 1 &&
            update.delete_known_present_index) {
            update.map_delete_makes_empty = true;
        } else if (map_is_singleton) {
            bool deletes_tracked_index = update.has_precise_index &&
                                         !update.index_may_alias_other_slots &&
                                         current_catch_has_index_aggregate_container(
                                             ctx, id, name, update.index, update.index_symbol_id,
                                             update.index_symbol_name, update.index_string);
            bool deletes_caught_key =
                current_catch_has_key_element_aggregate_container(ctx, id, name) &&
                is_current_caught_ref(ctx, call->arguments[0]);
            update.map_delete_makes_empty = deletes_tracked_index || deletes_caught_key;
        }
        update.action = CATCH_AGGREGATE_MEMBER_MAP_DELETE_PRECISE;
        return update;
    }
    return update;
}

static void apply_catch_aggregate_member_update(ErrorSetCtx *ctx,
                                                const CatchAggregateMemberUpdate *update) {
    if (!ctx || !update)
        return;
    switch (update->action) {
        case CATCH_AGGREGATE_MEMBER_PRESERVE:
            break;
        case CATCH_AGGREGATE_MEMBER_SET_ADD_CAUGHT:
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_EMPTY);
            add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                              CATCH_AGGREGATE_ELEMENT, -1, 0, NULL, NULL, NULL);
            break;
        case CATCH_AGGREGATE_MEMBER_SET_DELETE_CAUGHT:
            remove_current_catch_aggregate_aliases_for_container(ctx, update->container_id,
                                                                 update->container_name);
            add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                              CATCH_AGGREGATE_EMPTY, -1, 0, NULL, NULL, NULL);
            break;
        case CATCH_AGGREGATE_MEMBER_SET_CLEAR:
            remove_current_catch_aggregate_aliases_for_container(ctx, update->container_id,
                                                                 update->container_name);
            add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                              CATCH_AGGREGATE_EMPTY, -1, 0, NULL, NULL, NULL);
            break;
        case CATCH_AGGREGATE_MEMBER_MAP_SET_PRECISE:
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_EMPTY);
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_SINGLETON);
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KNOWN_COUNT);
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_PRESENT_INDEX);
            if (update->index_may_alias_other_slots)
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_INDEX);
            else if (update->has_precise_index)
                remove_current_catch_aggregate_alias_for_index(
                    ctx, update->container_id, update->container_name, update->index,
                    update->index_symbol_id, update->index_symbol_name, update->index_string);
            else
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_INDEX);
            if (update->value_is_caught) {
                if (update->has_precise_index)
                    add_current_catch_aggregate_alias(
                        ctx, update->container_id, update->container_name, CATCH_AGGREGATE_INDEX,
                        update->index, update->index_symbol_id, update->index_symbol_name,
                        update->index_string, NULL);
                if (update->value_set_starts_tracked_map)
                    add_current_catch_aggregate_alias(
                        ctx, update->container_id, update->container_name, CATCH_AGGREGATE_ELEMENT,
                        -1, 0, NULL, NULL, NULL);
            } else {
                remove_current_catch_aggregate_alias_for_element(ctx, update->container_id,
                                                                 update->container_name);
            }
            if (update->key_set_starts_tracked_map) {
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_KEY_ELEMENT, -1, 0, NULL, NULL,
                                                  NULL);
            } else if (!update->key_is_caught) {
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KEY_ELEMENT);
            }
            if (update->value_set_starts_tracked_map || update->key_set_starts_tracked_map)
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_SINGLETON, -1, 0, NULL, NULL,
                                                  NULL);
            break;
        case CATCH_AGGREGATE_MEMBER_MAP_DELETE_PRECISE:
            if (update->map_delete_makes_empty) {
                remove_current_catch_aggregate_aliases_for_container(ctx, update->container_id,
                                                                     update->container_name);
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_EMPTY, -1, 0, NULL, NULL, NULL);
                break;
            }
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_SINGLETON);
            remove_current_catch_aggregate_aliases_for_kind(
                ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KNOWN_COUNT);
            if (update->index_may_alias_other_slots)
                remove_current_catch_aggregate_aliases_for_kind(ctx, update->container_id,
                                                                update->container_name,
                                                                CATCH_AGGREGATE_PRESENT_INDEX);
            else if (update->has_precise_index)
                remove_current_catch_aggregate_alias_for_index_kind(
                    ctx, update->container_id, update->container_name,
                    CATCH_AGGREGATE_PRESENT_INDEX, update->index, update->index_symbol_id,
                    update->index_symbol_name, update->index_string);
            else
                remove_current_catch_aggregate_aliases_for_kind(ctx, update->container_id,
                                                                update->container_name,
                                                                CATCH_AGGREGATE_PRESENT_INDEX);
            if (update->index_may_alias_other_slots)
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_INDEX);
            else if (update->has_precise_index)
                remove_current_catch_aggregate_alias_for_index(
                    ctx, update->container_id, update->container_name, update->index,
                    update->index_symbol_id, update->index_symbol_name, update->index_string);
            else
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_INDEX);
            if (update->index_may_alias_other_slots)
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KEY_INDEX);
            else if (update->has_precise_index)
                remove_current_catch_aggregate_alias_for_index_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KEY_INDEX,
                    update->index, update->index_symbol_id, update->index_symbol_name,
                    update->index_string);
            else
                remove_current_catch_aggregate_aliases_for_kind(
                    ctx, update->container_id, update->container_name, CATCH_AGGREGATE_KEY_INDEX);
            if (update->has_known_count && update->known_count > 0 &&
                update->delete_known_present_index) {
                int64_t remaining_count = update->known_count - 1;
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_KNOWN_COUNT, remaining_count, 0,
                                                  NULL, NULL, NULL);
                if (remaining_count > 0 && current_catch_aggregate_alias_count_for_kind(
                                               ctx, update->container_id, update->container_name,
                                               CATCH_AGGREGATE_INDEX) == remaining_count) {
                    add_current_catch_aggregate_alias(
                        ctx, update->container_id, update->container_name, CATCH_AGGREGATE_ELEMENT,
                        -1, 0, NULL, NULL, NULL);
                    if (remaining_count == 1)
                        add_current_catch_aggregate_alias(
                            ctx, update->container_id, update->container_name,
                            CATCH_AGGREGATE_SINGLETON, -1, 0, NULL, NULL, NULL);
                }
                if (remaining_count > 0 && current_catch_aggregate_alias_count_for_kind(
                                               ctx, update->container_id, update->container_name,
                                               CATCH_AGGREGATE_KEY_INDEX) == remaining_count) {
                    add_current_catch_aggregate_alias(
                        ctx, update->container_id, update->container_name,
                        CATCH_AGGREGATE_KEY_ELEMENT, -1, 0, NULL, NULL, NULL);
                    if (remaining_count == 1)
                        add_current_catch_aggregate_alias(
                            ctx, update->container_id, update->container_name,
                            CATCH_AGGREGATE_SINGLETON, -1, 0, NULL, NULL, NULL);
                }
            }
            break;
        case CATCH_AGGREGATE_MEMBER_MAP_CLEAR: {
            bool had_caught_values = current_catch_has_element_aggregate_container(
                ctx, update->container_id, update->container_name);
            bool had_caught_keys = current_catch_has_key_element_aggregate_container(
                ctx, update->container_id, update->container_name);
            remove_current_catch_aggregate_aliases_for_container(ctx, update->container_id,
                                                                 update->container_name);
            add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                              CATCH_AGGREGATE_EMPTY, -1, 0, NULL, NULL, NULL);
            if (had_caught_values)
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_ELEMENT, -1, 0, NULL, NULL, NULL);
            if (had_caught_keys)
                add_current_catch_aggregate_alias(ctx, update->container_id, update->container_name,
                                                  CATCH_AGGREGATE_KEY_ELEMENT, -1, 0, NULL, NULL,
                                                  NULL);
            break;
        }
        case CATCH_AGGREGATE_MEMBER_INVALIDATE:
            remove_current_catch_aggregate_aliases_for_container(ctx, update->container_id,
                                                                 update->container_name);
            break;
    }
}

static bool catch_aggregate_decl_allows_tracking(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || (node->type != AST_CONST_DECL && node->type != AST_VAR_DECL))
        return false;
    if (node->type == AST_CONST_DECL)
        return true;
    VarDeclNode *decl = &node->as.var_decl;
    XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, decl->symbol_id);
    return sym && sym->kind == XA_SYM_VARIABLE && sym->links.assign_count == 1;
}

static void maybe_record_catch_aggregate_alias(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !ctx->current_caught ||
        (node->type != AST_CONST_DECL && node->type != AST_VAR_DECL))
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->name || !decl->initializer || decl->symbol_id == 0)
        return;

    uint32_t source_id = 0;
    const char *source_name = NULL;
    if (expr_references_catch_aggregate_container(ctx, decl->initializer, &source_id, &source_name))
        remove_current_catch_aggregate_aliases_for_container(ctx, source_id, source_name);

    if (!catch_aggregate_decl_allows_tracking(ctx, node))
        return;
    record_catch_aggregate_entries_from_initializer(ctx, decl->symbol_id, decl->name,
                                                    decl->initializer);
}

static void record_catch_alias_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign || !ctx->current_caught)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    const char *name = assign->name;
    if (symbol_id == 0 && !name)
        return;

    bool rhs_is_caught = is_current_caught_ref(ctx, assign->value);
    if (current_catch_target_matches(ctx, symbol_id, name)) {
        ctx->current_catch_binding_is_caught = rhs_is_caught;
        return;
    }

    int alias_index = current_catch_alias_index(ctx, symbol_id, name);
    if (alias_index < 0) {
        if (rhs_is_caught && sym && sym->kind == XA_SYM_VARIABLE && !sym->is_const &&
            sym->is_rebindable)
            add_current_catch_alias(ctx, symbol_id, name);
        return;
    }
    remove_current_catch_alias(ctx, alias_index);
    if (rhs_is_caught)
        add_current_catch_alias(ctx, symbol_id, name);
}

static void record_catch_destructure_aliases(ErrorSetCtx *ctx, XrDestructurePattern *pattern,
                                             AstNode *source) {
    if (!ctx || !pattern || !source || !ctx->current_caught)
        return;

    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            if (pattern->as.identifier.symbol_id != 0 && pattern->as.identifier.name &&
                is_current_caught_ref(ctx, source))
                add_current_catch_alias(ctx, pattern->as.identifier.symbol_id,
                                        pattern->as.identifier.name);
            return;

        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                AstNode index = {0};
                index.type = AST_LITERAL_INT;
                index.as.literal.kind = LITERAL_KIND_INT;
                index.as.literal.int_bits = (uint64_t) i;
                index.as.literal.raw_value.int_val = i;

                AstNode element = {0};
                element.type = AST_INDEX_GET;
                element.as.index_get.array = source;
                element.as.index_get.index = &index;
                record_catch_destructure_aliases(ctx, pattern->as.array.elements[i], &element);
            }
            return;

        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                const char *field =
                    pattern->as.object.field_names ? pattern->as.object.field_names[i] : NULL;
                if (!field)
                    continue;
                AstNode member = {0};
                member.type = AST_MEMBER_ACCESS;
                member.as.member_access.object = source;
                member.as.member_access.name = (char *) field;
                record_catch_destructure_aliases(ctx, pattern->as.object.patterns[i], &member);
            }
            return;

        default:
            return;
    }
}

static void record_catch_aggregate_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign || !ctx->current_caught)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    const char *name = assign->name;
    if (symbol_id == 0 && !name)
        return;

    uint32_t source_id = 0;
    const char *source_name = NULL;
    if (expr_references_catch_aggregate_container(ctx, assign->value, &source_id, &source_name))
        remove_current_catch_aggregate_aliases_for_container(ctx, source_id, source_name);

    remove_current_catch_aggregate_aliases_for_container(ctx, symbol_id, name);
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable)
        return;
    record_catch_aggregate_entries_from_initializer(ctx, symbol_id, name, assign->value);
}

static void record_catch_aggregate_index_set(ErrorSetCtx *ctx, IndexSetNode *set) {
    if (!ctx || !set || !ctx->current_caught)
        return;
    uint32_t container_id = 0;
    const char *container_name = NULL;
    if (!variable_ref_symbol(ctx, set->array, &container_id, &container_name))
        return;
    XrType *container_type = xa_analyzer_get_node_type(ctx->analyzer, set->array);
    bool is_map = container_type && XR_TYPE_IS_MAP(container_type);
    int64_t index = -1;
    uint32_t index_symbol_id = 0;
    const char *index_symbol_name = NULL;
    const char *index_string = NULL;
    bool rhs_is_caught = is_current_caught_ref(ctx, set->value);
    bool has_precise_index = catch_aggregate_index_key(ctx, set->index, &index, &index_symbol_id,
                                                       &index_symbol_name, &index_string);
    if (is_map) {
        bool key_is_caught = is_current_caught_ref(ctx, set->index);
        bool map_has_tracked_entries =
            current_catch_has_aggregate_container(ctx, container_id, container_name);
        bool map_is_known_empty =
            current_catch_has_empty_aggregate_container(ctx, container_id, container_name);
        bool key_set_starts_tracked_map = key_is_caught && map_is_known_empty;
        bool value_set_starts_tracked_map = rhs_is_caught && map_is_known_empty;
        bool index_may_alias_other_slots =
            has_precise_index && (index_symbol_id != 0 || index_symbol_name != NULL);

        if (!key_is_caught && !rhs_is_caught && !map_has_tracked_entries)
            return;

        remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                        CATCH_AGGREGATE_EMPTY);
        remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                        CATCH_AGGREGATE_SINGLETON);
        remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                        CATCH_AGGREGATE_KNOWN_COUNT);
        remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                        CATCH_AGGREGATE_PRESENT_INDEX);
        if (index_may_alias_other_slots) {
            remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                            CATCH_AGGREGATE_INDEX);
        } else if (has_precise_index) {
            remove_current_catch_aggregate_alias_for_index(ctx, container_id, container_name, index,
                                                           index_symbol_id, index_symbol_name,
                                                           index_string);
        } else {
            remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                            CATCH_AGGREGATE_INDEX);
        }

        if (rhs_is_caught) {
            if (has_precise_index)
                add_current_catch_aggregate_alias(ctx, container_id, container_name,
                                                  CATCH_AGGREGATE_INDEX, index, index_symbol_id,
                                                  index_symbol_name, index_string, NULL);
            if (value_set_starts_tracked_map)
                add_current_catch_aggregate_alias(ctx, container_id, container_name,
                                                  CATCH_AGGREGATE_ELEMENT, -1, 0, NULL, NULL, NULL);
        } else {
            remove_current_catch_aggregate_alias_for_element(ctx, container_id, container_name);
        }

        if (key_set_starts_tracked_map) {
            add_current_catch_aggregate_alias(ctx, container_id, container_name,
                                              CATCH_AGGREGATE_KEY_ELEMENT, -1, 0, NULL, NULL, NULL);
        } else if (!key_is_caught) {
            remove_current_catch_aggregate_aliases_for_kind(ctx, container_id, container_name,
                                                            CATCH_AGGREGATE_KEY_ELEMENT);
        }
        if (value_set_starts_tracked_map || key_set_starts_tracked_map)
            add_current_catch_aggregate_alias(ctx, container_id, container_name,
                                              CATCH_AGGREGATE_SINGLETON, -1, 0, NULL, NULL, NULL);
        return;
    }
    if (!has_precise_index) {
        remove_current_catch_aggregate_aliases_for_container(ctx, container_id, container_name);
        return;
    }
    CatchAggregateAlias alias = {.container_id = container_id,
                                 .container_name = container_name,
                                 .kind = CATCH_AGGREGATE_INDEX,
                                 .index = index,
                                 .index_symbol_id = index_symbol_id,
                                 .index_symbol_name = index_symbol_name,
                                 .index_string = index_string,
                                 .field_name = NULL};
    int alias_index = current_catch_aggregate_alias_index(ctx, &alias);
    if (alias_index >= 0)
        remove_current_catch_aggregate_alias(ctx, alias_index);
    if (rhs_is_caught)
        add_current_catch_aggregate_alias(ctx, container_id, container_name, CATCH_AGGREGATE_INDEX,
                                          index, index_symbol_id, index_symbol_name, index_string,
                                          NULL);
}

static void record_catch_aggregate_member_set(ErrorSetCtx *ctx, MemberSetNode *set) {
    if (!ctx || !set || !ctx->current_caught)
        return;
    uint32_t container_id = 0;
    const char *container_name = NULL;
    if (!variable_ref_symbol(ctx, set->object, &container_id, &container_name))
        return;
    bool rhs_is_caught = is_current_caught_ref(ctx, set->value);
    CatchAggregateAlias alias = {.container_id = container_id,
                                 .container_name = container_name,
                                 .kind = CATCH_AGGREGATE_FIELD,
                                 .index = -1,
                                 .field_name = set->member};
    int alias_index = current_catch_aggregate_alias_index(ctx, &alias);
    if (alias_index >= 0)
        remove_current_catch_aggregate_alias(ctx, alias_index);
    if (rhs_is_caught && set->member)
        add_current_catch_aggregate_alias(ctx, container_id, container_name, CATCH_AGGREGATE_FIELD,
                                          -1, 0, NULL, NULL, set->member);
}

/* Try to find the case index of an enum member.
 * Searches the enum symbol's member list. */
static int find_enum_case_index(XaSymbol *enum_sym, const char *case_name) {
    if (!enum_sym || !case_name)
        return -1;
    XaSymbolLinks *links = &enum_sym->links;
    return xa_enum_info_find_variant(links->enum_info, case_name);
}

static XaSymbol *lookup_enum_symbol(XaAnalyzer *analyzer, const char *enum_name) {
    if (!analyzer || !enum_name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, enum_name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_in_scope(analyzer, enum_name, analyzer->global_scope);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_deep(analyzer, enum_name);
    return (sym && sym->kind == XA_SYM_ENUM) ? sym : NULL;
}

static bool copy_contract_ident_segment(char *dst, size_t dst_size, const char *start,
                                        const char *end) {
    if (!dst || dst_size == 0 || !start || !end || end <= start)
        return false;
    const char *segment = start;
    for (const char *p = start; p < end; p++) {
        if (*p == '.')
            segment = p + 1;
        else if (*p == ':' && p + 1 < end && p[1] == ':')
            segment = p + 2;
    }
    size_t len = (size_t) (end - segment);
    if (len == 0 || len >= dst_size)
        return false;
    memcpy(dst, segment, len);
    dst[len] = '\0';
    return true;
}

static bool split_effect_contract_ref(const char *ref, char *enum_name, size_t enum_name_size,
                                      char *variant_name, size_t variant_name_size,
                                      bool *has_variant) {
    if (!ref || !enum_name || !variant_name || !has_variant)
        return false;
    const char *end = ref + strlen(ref);
    const char *dot = strrchr(ref, '.');
    *has_variant = dot && dot + 1 < end;
    if (*has_variant) {
        if (!copy_contract_ident_segment(enum_name, enum_name_size, ref, dot))
            return false;
        size_t variant_len = (size_t) (end - (dot + 1));
        if (variant_len == 0 || variant_len >= variant_name_size)
            return false;
        memcpy(variant_name, dot + 1, variant_len);
        variant_name[variant_len] = '\0';
    } else {
        if (!copy_contract_ident_segment(enum_name, enum_name_size, ref, end))
            return false;
        variant_name[0] = '\0';
    }
    return true;
}

static bool es_apply_effect_contract(ErrorSetCtx *ctx, const XaEffectContract *contract) {
    if (!ctx || !ctx->current_summary || !contract)
        return false;
    if (contract->kind == XA_EFFECT_CONTRACT_NOTHROW)
        return true;
    if (contract->kind == XA_EFFECT_CONTRACT_MISSING) {
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_NATIVE_CONTRACT_MISSING);
        return true;
    }

    bool complete = true;
    for (uint32_t i = 0; i < contract->error_count; i++) {
        char enum_name[128];
        char variant_name[128];
        bool has_variant = false;
        if (!split_effect_contract_ref(contract->errors[i], enum_name, sizeof(enum_name),
                                       variant_name, sizeof(variant_name), &has_variant)) {
            complete = false;
            continue;
        }
        XaSymbol *enum_sym = lookup_enum_symbol(ctx->analyzer, enum_name);
        XrType *enum_type =
            (enum_sym && enum_sym->kind == XA_SYM_ENUM) ? enum_sym->links.type : NULL;
        if (!enum_type || !XR_TYPE_IS_ENUM(enum_type)) {
            complete = false;
            continue;
        }
        if (!has_variant) {
            es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary, enum_type);
            continue;
        }
        int case_index = find_enum_case_index(enum_sym, variant_name);
        if (case_index < 0) {
            complete = false;
            continue;
        }
        es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary, enum_type,
                                 (uint32_t) case_index);
    }
    if (!complete)
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_NATIVE_CONTRACT_MISSING);
    return true;
}

static const XaEffectContract *es_module_member_effect_contract(ErrorSetCtx *ctx, AstNode *callee) {
    if (!ctx || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE ||
        !ma->object->as.variable.name)
        return NULL;
    XaSymbol *mod_sym = lookup_variable_symbol(ctx->analyzer, ma->object);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *module_name =
        (links && links->module_name) ? links->module_name : ma->object->as.variable.name;
    return xa_builtin_get_module_func_effect_contract(module_name, ma->name);
}

static const XaEffectContract *es_imported_function_effect_contract(ErrorSetCtx *ctx,
                                                                    AstNode *callee) {
    if (!ctx || !callee || callee->type != AST_VARIABLE)
        return NULL;
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, callee);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->module_name || !links->import_member_name)
        return NULL;
    /* Selectively imported public functions use the public lookup above.  A
     * bodyless native primitive injected into a stdlib source module is an ABI
     * symbol: it is intentionally hidden from user lookup, but its generated
     * error contract remains authoritative for the Xray wrapper that calls it. */
    return xa_builtin_get_module_func_abi_effect_contract(links->module_name,
                                                          links->import_member_name);
}

static const XaEffectContract *es_handle_method_effect_contract(ErrorSetCtx *ctx, AstNode *callee) {
    if (!ctx || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return NULL;
    XrType *receiver_type = xa_analyzer_get_node_type(ctx->analyzer, ma->object);
    const char *handle_name = receiver_type && XR_TYPE_IS_INSTANCE(receiver_type)
                                  ? receiver_type->instance.class_name
                                  : NULL;
    if (!handle_name)
        return NULL;
    return xa_builtin_get_handle_method_effect_contract(handle_name, ma->name);
}

/* Error contract of a receiver-specialized builtin intrinsic.
 *
 * The intrinsic table (xbuiltin_receiver_method.def) carries signatures and
 * memory effects but no error contract, so without this lookup every
 * `arr.clear()` / `arr.push(x)` left the caller's effect summary incomplete and
 * nothing that touches a collection could ever be proven no-throw — including
 * the cleanup idioms spec 8.3.1 rule D1 has to accept.
 *
 * These are Array / Slice / exact-integer primitives implemented in C: they
 * raise panics, never value-return errors (see
 * xa_builtin_receiver_method_is_nothrow). The higher-order ones are excluded
 * there because they re-raise whatever their callback throws, so they stay
 * unproven and the caller's summary stays incomplete — fail-closed, per 8.0. */
static const XaEffectContract *es_receiver_intrinsic_effect_contract(XrType *receiver_type,
                                                                     const char *name) {
    if (!receiver_type || !name)
        return NULL;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (!xa_builtin_receiver_matches_type(receiver_type, spec->receiver) ||
            strcmp(spec->source_name, name) != 0)
            continue;
        if (!xa_builtin_receiver_method_is_nothrow(spec))
            return NULL;
        static const XaEffectContract nothrow = {
            .kind = XA_EFFECT_CONTRACT_NOTHROW,
            .errors = NULL,
            .error_count = 0,
        };
        return &nothrow;
    }
    return NULL;
}

static const XaEffectContract *es_builtin_type_member_effect_contract(ErrorSetCtx *ctx,
                                                                      AstNode *callee) {
    if (!ctx || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return NULL;

    const XaEffectContract *contract = NULL;
    if (ma->object->type == AST_VARIABLE && ma->object->as.variable.name) {
        XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, ma->object);
        if (!sym || sym->kind != XA_SYM_MODULE) {
            contract = xa_builtin_get_named_type_member_effect_contract(
                ma->object->as.variable.name, ma->name, true);
            if (contract)
                return contract;
        }
    }

    XrType *receiver_type = xa_analyzer_get_node_type(ctx->analyzer, ma->object);
    contract = es_receiver_intrinsic_effect_contract(receiver_type, ma->name);
    if (contract)
        return contract;
    contract = xa_builtin_get_type_member_effect_contract(receiver_type, ma->name, false);
    return contract;
}

static bool es_apply_native_call_contract(ErrorSetCtx *ctx, AstNode *callee) {
    AstNode *source = identity_source(callee);
    const XaEffectContract *contract = es_imported_function_effect_contract(ctx, source);
    if (!contract)
        contract = es_module_member_effect_contract(ctx, source);
    if (!contract)
        contract = es_handle_method_effect_contract(ctx, source);
    if (!contract)
        contract = es_builtin_type_member_effect_contract(ctx, source);
    return es_apply_effect_contract(ctx, contract);
}

/* ========== Coroutine Boundaries ========== */

/* An error keeps its channel when it crosses an execution boundary, so it also
 * keeps its place in the error set: whoever re-raises a coroutine's failure --
 * `await t` in the awaiting frame, `linked scope` at its exit -- is as fallible
 * as the coroutine body itself.  Charge those errors to the walking function so
 * lowering emits the ERR_CHECK that routes them to `catch (e)`, and so callers
 * see the function as fallible.  A body we cannot name is fail-closed: unknown,
 * therefore may-throw. */

/* Union the error set of the function a spawn expression starts.  `spawn_expr`
 * is a `go`'s operand: `go f(x)` (a call) or `go closure` (a function value). */
static void es_union_spawned_body_effects(ErrorSetCtx *ctx, AstNode *spawn_expr) {
    if (!ctx || !spawn_expr) {
        xa_effect_summary_mark_incomplete(ctx ? ctx->current_summary : NULL,
                                          XA_UNKNOWN_UNRESOLVED_CALLEE);
        return;
    }

    const CallExprNode *call = spawn_expr->type == AST_CALL_EXPR ? &spawn_expr->as.call_expr : NULL;
    AstNode *callee = call ? call->callee : spawn_expr;

    if (es_walk_immediate_function_expr_call(ctx, callee))
        return;
    if (es_apply_native_call_contract(ctx, callee))
        return;

    FunctionValueTarget target = resolve_call_target(ctx, callee);
    if (!function_value_target_is_exact(target)) {
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_DYNAMIC_CALL_TARGET);
        return;
    }

    for (int i = 0; i < target.target_count; i++) {
        AstNode *function_expr = target.target_function_exprs[i];
        XaSymbol *callee_sym = target.target_symbols[i];
        if (function_expr) {
            es_walk_function_expr_body(ctx, function_expr);
            continue;
        }
        if (call && es_walk_callsite_function_decl_body(ctx, callee_sym, call))
            continue;
        if (callee_sym &&
            (callee_sym->kind == XA_SYM_FUNCTION || callee_sym->kind == XA_SYM_METHOD) &&
            callee_sym->links.effect_id != XA_EFFECT_NONE) {
            const XaEffectSummary *callee_summary =
                xa_effect_db_get(ctx->analyzer->effect_db, callee_sym->links.effect_id);
            if (callee_summary) {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, ctx->current_summary,
                                              callee_summary);
                continue;
            }
        }
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_UNRESOLVED_CALLEE);
    }
}

static void set_task_spawn_alias(ErrorSetCtx *ctx, uint32_t symbol_id, AstNode *spawn_expr) {
    if (!ctx || symbol_id == 0)
        return;
    for (int i = 0; i < ctx->task_spawn_alias_count; i++) {
        if (ctx->task_spawn_alias_ids[i] == symbol_id) {
            ctx->task_spawn_alias_exprs[i] = spawn_expr;
            return;
        }
    }
    if (ctx->task_spawn_alias_count >= 64)
        return;
    int slot = ctx->task_spawn_alias_count++;
    ctx->task_spawn_alias_ids[slot] = symbol_id;
    ctx->task_spawn_alias_exprs[slot] = spawn_expr;
}

/* NULL means "no known spawn": either never bound here, or invalidated by a
 * rebind we could not follow. */
static AstNode *task_spawn_alias_expr(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || symbol_id == 0)
        return NULL;
    for (int i = 0; i < ctx->task_spawn_alias_count; i++) {
        if (ctx->task_spawn_alias_ids[i] == symbol_id)
            return ctx->task_spawn_alias_exprs[i];
    }
    return NULL;
}

/* The spawn expression behind an awaited operand, or NULL when it cannot be
 * named from here (a task read out of an array, passed in as a parameter, or
 * returned by a call). */
static AstNode *awaited_spawn_expr(ErrorSetCtx *ctx, AstNode *awaited) {
    AstNode *source = identity_source(awaited);
    if (!source)
        return NULL;
    if (source->type == AST_GO_EXPR)
        return source->as.go_expr.expr;
    if (source->type == AST_VARIABLE) {
        XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, source);
        return task_spawn_alias_expr(ctx, sym ? sym->id : 0);
    }
    return NULL;
}

/* Only a plain `await t` re-raises.  `await any` / `await all` / `await
 * anySuccess` aggregate outcomes into a value, `await t timeout ms` yields null
 * on failure, and `into` collects into a caller-provided slot -- none of them
 * put the child's error back on an error channel. */
static bool await_expr_rethrows(const AwaitExprNode *aw) {
    return aw && !aw->is_any && !aw->is_all && !aw->is_any_success && !aw->timeout && !aw->into;
}

/* `var t = go f()` — remember the spawn so a later `await t` can find f. */
static void maybe_record_task_spawn_var_initializer(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_VAR_DECL)
        return;
    VarDeclNode *decl = &node->as.var_decl;
    AstNode *init = identity_source(decl->initializer);
    if (!init || init->type != AST_GO_EXPR || decl->symbol_id == 0)
        return;
    set_task_spawn_alias(ctx, decl->symbol_id, init->as.go_expr.expr);
}

/* `t = go g()` rebinds; `t = <anything else>` makes the binding unknowable from
 * here.  Both must land in the table -- dropping the second would leave the
 * stale spawn behind and understate the error set. */
static void record_task_spawn_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    if (symbol_id == 0)
        return;
    AstNode *value = identity_source(assign->value);
    set_task_spawn_alias(ctx, symbol_id,
                         value && value->type == AST_GO_EXPR ? value->as.go_expr.expr : NULL);
}

/* ========== Expression Walking ========== */

static bool es_walk_enter(ErrorSetCtx *ctx) {
    if (ctx->ast_walk_depth >= ERROR_SET_AST_WALK_MAX) {
        xa_effect_summary_mark_incomplete(ctx->current_summary, XA_UNKNOWN_ANALYSIS_LIMIT);
        return false;
    }
    ctx->ast_walk_depth++;
    return true;
}

static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !es_walk_enter(ctx))
        return;
    es_walk_expr_inner(ctx, node);
    ctx->ast_walk_depth--;
}

static void es_walk_expr_inner(ErrorSetCtx *ctx, AstNode *node) {
    switch (node->type) {
        case AST_CALL_EXPR: {
            /* Walk arguments first */
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                es_walk_expr(ctx, node->as.call_expr.arguments[i]);
                if (!call_arg_preserves_catch_aggregate_ref(ctx, &node->as.call_expr, i))
                    invalidate_catch_aggregate_container_ref(ctx, node->as.call_expr.arguments[i]);
            }
            es_walk_expr(ctx, node->as.call_expr.callee);
            AstNode *callee_source = identity_source(node->as.call_expr.callee);
            CatchAggregateMemberUpdate member_update =
                catch_aggregate_member_update(ctx, node->as.call_expr.callee, &node->as.call_expr);
            if (callee_source && callee_source->type == AST_MEMBER_ACCESS)
                apply_catch_aggregate_member_update(ctx, &member_update);

            if (es_walk_immediate_function_expr_call(ctx, node->as.call_expr.callee))
                break;
            if (es_apply_native_call_contract(ctx, node->as.call_expr.callee))
                break;

            const XaSelection *callee_selection =
                xa_analyzer_get_selection(ctx->analyzer, callee_source);
            bool open_dispatch = method_selection_has_open_virtual_dispatch(callee_selection);
            bool exact_open_dispatch = false;
            FunctionValueTarget call_target =
                open_dispatch ? resolve_open_virtual_dispatch_targets(ctx, callee_selection,
                                                                      &exact_open_dispatch)
                              : function_value_target_none();
            if (open_dispatch && !exact_open_dispatch) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH);
            }

            /* Union callee's effect summary into current function's summary */
            if (!function_value_target_is_exact(call_target))
                call_target = resolve_call_target(ctx, node->as.call_expr.callee);
            if (function_value_target_is_exact(call_target)) {
                for (int i = 0; i < call_target.target_count; i++) {
                    AstNode *function_expr = call_target.target_function_exprs[i];
                    XaSymbol *callee_sym = call_target.target_symbols[i];
                    if (function_expr) {
                        es_walk_function_expr_body(ctx, function_expr);
                        continue;
                    }
                    record_specialized_function_call_targets(ctx, callee_sym, &node->as.call_expr);
                    if (es_walk_callsite_function_decl_body(ctx, callee_sym, &node->as.call_expr))
                        continue;
                    if (callee_sym &&
                        (callee_sym->kind == XA_SYM_FUNCTION ||
                         callee_sym->kind == XA_SYM_METHOD) &&
                        callee_sym->links.effect_id != XA_EFFECT_NONE) {
                        const XaEffectSummary *callee_summary =
                            xa_effect_db_get(ctx->analyzer->effect_db, callee_sym->links.effect_id);
                        if (callee_summary)
                            xa_effect_summary_add_summary(ctx->analyzer->effect_db,
                                                          ctx->current_summary, callee_summary);
                    }
                }
                break;
            }
            if (is_dynamic_function_call_target(ctx->analyzer, node->as.call_expr.callee,
                                                call_target.symbol)) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_DYNAMIC_CALL_TARGET);
            }
            break;
        }

        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            es_walk_expr(ctx, node->as.binary.left);
            es_walk_expr(ctx, node->as.binary.right);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            es_walk_expr(ctx, node->as.unary.operand);
            break;

        case AST_TERNARY:
            es_walk_expr(ctx, node->as.ternary.condition);
            es_walk_expr(ctx, node->as.ternary.true_expr);
            es_walk_expr(ctx, node->as.ternary.false_expr);
            break;

        case AST_MEMBER_ACCESS:
            es_walk_expr(ctx, node->as.member_access.object);
            break;

        case AST_INDEX_GET:
            es_walk_expr(ctx, node->as.index_get.array);
            es_walk_expr(ctx, node->as.index_get.index);
            break;

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                es_walk_expr(ctx, node->as.array_literal.repeat_value);
                es_walk_expr(ctx, node->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    es_walk_expr(ctx, node->as.array_literal.elements[i]);
            }
            break;

        case AST_TUPLE_LITERAL:
            for (int i = 0; i < node->as.tuple_literal.count; i++)
                es_walk_expr(ctx, node->as.tuple_literal.elements[i]);
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                es_walk_expr(ctx, node->as.object_literal.values[i]);
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                es_walk_expr(ctx, node->as.map_literal.keys[i]);
                es_walk_expr(ctx, node->as.map_literal.values[i]);
            }
            break;

        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++)
                es_walk_expr(ctx, node->as.set_literal.elements[i]);
            break;

        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                es_walk_expr(ctx, node->as.struct_literal.field_values[i]);
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                es_walk_expr(ctx, node->as.template_str.parts[i]);
            break;

        case AST_MATCH_EXPR:
            es_walk_expr(ctx, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                AstNode *arm = node->as.match_expr.arms[i];
                if (arm)
                    es_walk_expr(ctx, arm->as.match_arm.body);
            }
            break;

        case AST_NULLISH_COALESCE:
            es_walk_expr(ctx, node->as.binary.left);
            es_walk_expr(ctx, node->as.binary.right);
            break;

        case AST_FUNCTION_EXPR:
            /* Lambda: don't propagate its errors to the enclosing function. */
            break;

        case AST_GO_EXPR: {
            /* Spawning is not itself fallible, and a detached child's failure
             * is nobody's error until someone awaits it.  Inside a `linked
             * scope` someone does: the scope re-raises the first child failure
             * at its exit, in this frame. */
            AstNode *spawned = node->as.go_expr.expr;
            if (spawned && spawned->type == AST_CALL_EXPR) {
                for (int i = 0; i < spawned->as.call_expr.arg_count; i++)
                    es_walk_expr(ctx, spawned->as.call_expr.arguments[i]);
                es_walk_expr(ctx, spawned->as.call_expr.callee);
            } else {
                es_walk_expr(ctx, spawned);
            }
            if (ctx->linked_scope_depth > 0)
                es_union_spawned_body_effects(ctx, spawned);
            break;
        }

        case AST_AWAIT_EXPR: {
            AwaitExprNode *aw = &node->as.await_expr;
            es_walk_expr(ctx, aw->expr);
            es_walk_expr(ctx, aw->timeout);
            es_walk_expr(ctx, aw->into);
            if (!await_expr_rethrows(aw))
                break;
            AstNode *spawn_expr = awaited_spawn_expr(ctx, aw->expr);
            if (!spawn_expr) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_DYNAMIC_CALL_TARGET);
                break;
            }
            es_union_spawned_body_effects(ctx, spawn_expr);
            break;
        }

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_catch_aggregate_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
            record_task_spawn_assignment(ctx, &node->as.assignment);
            break;

        case AST_INDEX_SET:
            es_walk_expr(ctx, node->as.index_set.array);
            es_walk_expr(ctx, node->as.index_set.index);
            es_walk_expr(ctx, node->as.index_set.value);
            record_catch_aggregate_index_set(ctx, &node->as.index_set);
            break;

        case AST_MEMBER_SET:
            es_walk_expr(ctx, node->as.member_set.object);
            es_walk_expr(ctx, node->as.member_set.value);
            record_catch_aggregate_member_set(ctx, &node->as.member_set);
            break;

        case AST_GROUPING:
            es_walk_expr(ctx, node->as.grouping);
            break;

        default:
            break;
    }
}

/* ========== Statement Walking ========== */

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !es_walk_enter(ctx))
        return;
    es_walk_stmt_inner(ctx, node);
    ctx->ast_walk_depth--;
}

static void es_walk_block(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;
    if (node->type == AST_BLOCK) {
        int saved_catch_alias_count = ctx->current_catch_alias_count;
        int saved_catch_aggregate_alias_count = ctx->current_catch_aggregate_alias_count;
        int saved_function_value_alias_count = ctx->function_value_alias_count;
        for (int i = 0; i < node->as.block.count; i++)
            es_walk_stmt(ctx, node->as.block.statements[i]);
        finish_catch_alias_block(ctx, node, saved_catch_alias_count,
                                 saved_catch_aggregate_alias_count);
        ctx->function_value_alias_count = saved_function_value_alias_count;
    } else {
        es_walk_stmt(ctx, node);
    }
}

static void es_walk_stmt_inner(ErrorSetCtx *ctx, AstNode *node) {
    switch (node->type) {
        case AST_THROW_STMT: {
            AstNode *expr = node->as.throw_stmt.expression;
            if (!expr)
                break;

            es_walk_expr(ctx, expr);
            if (is_current_caught_ref(ctx, expr)) {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, ctx->current_summary,
                                              ctx->current_caught);
                break;
            }

            const XaSelection *throw_sel = xa_analyzer_get_selection(ctx->analyzer, expr);
            if (es_summary_add_enum_selection(ctx, throw_sel))
                break;
            if (expr->type == AST_CALL_EXPR &&
                es_summary_add_enum_selection(
                    ctx, xa_analyzer_get_selection(ctx->analyzer, expr->as.call_expr.callee)))
                break;

            /*
             * Handle `throw EnumName.CaseName` — add the specific case.
             * Handle `throw variable` where variable has enum type — add all cases.
             */
            if (expr->type == AST_ENUM_ACCESS) {
                const char *enum_name = expr->as.enum_access.enum_name;
                const char *member_name = expr->as.enum_access.member_name;
                const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, expr);
                XaSymbol *enum_sym = NULL;
                XrType *enum_type = NULL;
                int case_idx = -1;
                if (sel && sel->kind == XA_SEL_ENUM_MEMBER) {
                    enum_sym = sel->target_symbol;
                    enum_type = sel->result_type;
                    case_idx = sel->field_index;
                }
                if (!enum_sym && enum_name)
                    enum_sym = lookup_enum_symbol(ctx->analyzer, enum_name);
                if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                    if (!enum_type)
                        enum_type = enum_sym->links.type;
                    if (case_idx < 0)
                        case_idx = find_enum_case_index(enum_sym, member_name);
                    if (enum_type && case_idx >= 0) {
                        es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary,
                                                 enum_type, (uint32_t) case_idx);
                    } else if (enum_type) {
                        es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary,
                                                enum_type);
                    }
                }
            } else {
                /* Generic throw: infer type from the expression's analyzed type */
                XrType *thrown_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
                if (thrown_type && XR_TYPE_IS_ENUM(thrown_type)) {
                    es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary,
                                            thrown_type);
                }
            }
            break;
        }

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;

            ErrorSetTryWalkState *walk =
                (ErrorSetTryWalkState *) xr_calloc(1, sizeof(ErrorSetTryWalkState));
            if (!walk) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                break;
            }
            xa_effect_summary_init(&walk->try_summary);
            XaEffectSummary *outer_summary = ctx->current_summary;
            bool exact_function_values = ctx->function_value_control_depth == 0;
            FunctionValueAliasState *catch_states = NULL;
            bool exact_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState *catch_alias_catch_states = NULL;
            bool *catch_alias_catch_reachable = NULL;
            XaEffectSummary *caught_summaries = NULL;

            if (tc->catch_count > 0) {
                if (exact_function_values) {
                    catch_states = (FunctionValueAliasState *) xr_calloc(
                        (size_t) tc->catch_count, sizeof(FunctionValueAliasState));
                }
                if (exact_catch_aliases) {
                    catch_alias_catch_states = (CatchAliasState *) xr_calloc(
                        (size_t) tc->catch_count, sizeof(CatchAliasState));
                    catch_alias_catch_reachable =
                        (bool *) xr_calloc((size_t) tc->catch_count, sizeof(bool));
                }
                caught_summaries = (XaEffectSummary *) xr_calloc((size_t) tc->catch_count,
                                                                 sizeof(XaEffectSummary));
                if ((exact_function_values && !catch_states) ||
                    (exact_catch_aliases &&
                     (!catch_alias_catch_states || !catch_alias_catch_reachable)) ||
                    !caught_summaries) {
                    xa_effect_summary_mark_incomplete(outer_summary,
                                                      XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                    xr_free(catch_states);
                    xr_free(catch_alias_catch_states);
                    xr_free(catch_alias_catch_reachable);
                    xr_free(caught_summaries);
                    xa_effect_summary_clear(&walk->try_summary);
                    xr_free(walk);
                    break;
                }
                for (int i = 0; i < tc->catch_count; i++)
                    xa_effect_summary_init(&caught_summaries[i]);
            }

            ctx->current_summary = &walk->try_summary;
            if (exact_catch_aliases)
                capture_catch_alias_state(ctx, &walk->base_catch_aliases);
            if (exact_function_values) {
                int mutation_start = ctx->function_value_mutation_count;
                int mutation_depth_before = ctx->function_value_mutation_depth;

                capture_function_value_alias_state(ctx, &walk->base_function_values);
                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (exact_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                ctx->function_value_mutation_depth++;
                es_walk_block(ctx, tc->try_body);
                ctx->function_value_mutation_depth--;
                if (exact_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->try_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->try_function_values);

                for (int i = mutation_start; i < ctx->function_value_mutation_count; i++) {
                    uint32_t id = ctx->function_value_mutation_ids[i];
                    if (id == 0)
                        continue;
                    bool seen = false;
                    for (int j = 0; j < walk->try_mutation_count; j++) {
                        if (walk->try_mutation_ids[j] == id) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen && walk->try_mutation_count < 128)
                        walk->try_mutation_ids[walk->try_mutation_count++] = id;
                }
                if (mutation_depth_before == 0)
                    ctx->function_value_mutation_count = mutation_start;

            } else {
                ctx->function_value_control_depth++;
                if (exact_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, tc->try_body);
                if (exact_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->try_catch_aliases);
                ctx->function_value_control_depth--;
            }
            ctx->current_summary = outer_summary;

            if (tc->catch_count > 0) {
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (!cc || cc->is_panic)
                        continue;
                    CatchEffectPattern cep = catch_effect_pattern(ctx->analyzer, cc);
                    if (cep.catch_all) {
                        if (caught_summaries)
                            xa_effect_summary_add_summary(ctx->analyzer->effect_db,
                                                          &caught_summaries[i], &walk->try_summary);
                        xa_effect_summary_clear_escaping(&walk->try_summary);
                        continue;
                    }
                    if (cep.has_enum) {
                        if (caught_summaries)
                            (cep.has_variant ? xa_effect_summary_add_variant_from_summary(
                                                   ctx->analyzer->effect_db, &caught_summaries[i],
                                                   &walk->try_summary, cep.type_id, cep.variant_id)
                                             : xa_effect_summary_add_type_from_summary(
                                                   ctx->analyzer->effect_db, &caught_summaries[i],
                                                   &walk->try_summary, cep.type_id));
                        if (cep.has_variant)
                            xa_effect_summary_subtract_variant(ctx->analyzer->effect_db,
                                                               &walk->try_summary, cep.type_id,
                                                               cep.variant_id);
                        else
                            xa_effect_summary_subtract_type(&walk->try_summary, cep.type_id);
                    }
                }
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, outer_summary,
                                              &walk->try_summary);
                if (exact_catch_aliases && caught_summaries && catch_alias_catch_reachable) {
                    for (int i = 0; i < tc->catch_count; i++) {
                        catch_alias_catch_reachable[i] =
                            !xa_effect_summary_is_nothrow(&caught_summaries[i]);
                    }
                }
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (cc && cc->body) {
                        if (exact_catch_aliases)
                            restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                        const char *saved_catch_var = ctx->current_catch_var;
                        uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
                        capture_catch_alias_state(ctx, &walk->saved_catch_aliases);
                        XaEffectSummary *saved_caught = ctx->current_caught;
                        ctx->current_catch_var = cc->var_name;
                        ctx->current_catch_symbol_id = cc->symbol_id;
                        ctx->current_catch_binding_is_caught = true;
                        ctx->current_catch_alias_count = 0;
                        ctx->current_caught = caught_summaries ? &caught_summaries[i] : NULL;
                        if (exact_function_values)
                            restore_function_value_alias_state_for_catch_entry(
                                ctx, &walk->base_function_values, walk->try_mutation_ids,
                                walk->try_mutation_count);
                        else
                            ctx->function_value_control_depth++;
                        es_walk_block(ctx, cc->body);
                        if (exact_function_values && catch_states)
                            capture_function_value_alias_state(ctx, &catch_states[i]);
                        if (!exact_function_values)
                            ctx->function_value_control_depth--;
                        ctx->current_catch_var = saved_catch_var;
                        ctx->current_catch_symbol_id = saved_catch_symbol_id;
                        restore_catch_alias_state(ctx, &walk->saved_catch_aliases);
                        ctx->current_caught = saved_caught;
                        if (exact_catch_aliases && catch_alias_catch_states)
                            capture_catch_alias_state(ctx, &catch_alias_catch_states[i]);
                    } else if (exact_function_values && catch_states) {
                        restore_function_value_alias_state_for_catch_entry(
                            ctx, &walk->base_function_values, walk->try_mutation_ids,
                            walk->try_mutation_count);
                        capture_function_value_alias_state(ctx, &catch_states[i]);
                        if (exact_catch_aliases && catch_alias_catch_states) {
                            restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                            capture_catch_alias_state(ctx, &catch_alias_catch_states[i]);
                        }
                    } else if (exact_catch_aliases && catch_alias_catch_states) {
                        restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                        capture_catch_alias_state(ctx, &catch_alias_catch_states[i]);
                    }
                }
                if (caught_summaries) {
                    for (int i = 0; i < tc->catch_count; i++)
                        xa_effect_summary_clear(&caught_summaries[i]);
                    xr_free(caught_summaries);
                }
            } else {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, outer_summary,
                                              &walk->try_summary);
            }
            if (exact_function_values) {
                if (tc->catch_count > 0 && catch_states) {
                    const FunctionValueAliasState *paths[129];
                    int path_count = 0;
                    paths[path_count++] = &walk->try_function_values;
                    for (int i = 0; i < tc->catch_count && path_count < 129; i++)
                        paths[path_count++] = &catch_states[i];
                    merge_function_value_path_states(ctx, &walk->base_function_values, paths,
                                                     path_count);
                } else {
                    restore_function_value_alias_state(ctx, &walk->try_function_values);
                }
            }
            if (exact_catch_aliases) {
                if (tc->catch_count > 0 && catch_alias_catch_states) {
                    const CatchAliasState *paths[129];
                    int path_count = 0;
                    paths[path_count++] = &walk->try_catch_aliases;
                    for (int i = 0; i < tc->catch_count && path_count < 129; i++) {
                        if (catch_alias_catch_reachable && !catch_alias_catch_reachable[i])
                            continue;
                        paths[path_count++] = &catch_alias_catch_states[i];
                    }
                    merge_catch_alias_path_states(ctx, paths, path_count);
                } else {
                    restore_catch_alias_state(ctx, &walk->try_catch_aliases);
                }
            }
            if (catch_states)
                xr_free(catch_states);
            if (catch_alias_catch_states)
                xr_free(catch_alias_catch_states);
            if (catch_alias_catch_reachable)
                xr_free(catch_alias_catch_reachable);
            xa_effect_summary_clear(&walk->try_summary);
            xr_free(walk);
            break;
        }

        case AST_EXPR_STMT:
            es_walk_expr(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
            maybe_record_catch_alias(ctx, node);
            maybe_record_catch_aggregate_alias(ctx, node);
            maybe_record_function_value_var_initializer(ctx, node);
            maybe_record_task_spawn_var_initializer(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_CONST_DECL:
            maybe_record_catch_alias(ctx, node);
            maybe_record_catch_aggregate_alias(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;
        case AST_DESTRUCTURE_DECL:
            record_catch_destructure_aliases(ctx, node->as.destructure_decl.pattern,
                                             node->as.destructure_decl.initializer);
            es_walk_expr(ctx, node->as.destructure_decl.initializer);
            break;

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_catch_aggregate_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
            record_task_spawn_assignment(ctx, &node->as.assignment);
            break;

        case AST_INDEX_SET:
            es_walk_expr(ctx, node->as.index_set.array);
            es_walk_expr(ctx, node->as.index_set.index);
            es_walk_expr(ctx, node->as.index_set.value);
            record_catch_aggregate_index_set(ctx, &node->as.index_set);
            break;

        case AST_MEMBER_SET:
            es_walk_expr(ctx, node->as.member_set.object);
            es_walk_expr(ctx, node->as.member_set.value);
            record_catch_aggregate_member_set(ctx, &node->as.member_set);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                es_walk_expr(ctx, node->as.return_stmt.values[i]);
            if (ctx->current_func) {
                if (node->as.return_stmt.value_count == 1) {
                    AstNode *value = node->as.return_stmt.values[0];
                    FunctionValueTarget target = resolve_function_value_expr_target(ctx, value, 0);
                    if (function_value_target_is_exact(target)) {
                        record_current_function_return_target(ctx, target, false);
                    } else if (expr_has_function_type(ctx, value) ||
                               current_function_returns_function(ctx)) {
                        record_current_function_return_target(ctx, function_value_target_none(),
                                                              true);
                    }
                } else if (current_function_returns_function(ctx)) {
                    record_current_function_return_target(ctx, function_value_target_none(), true);
                }
            }
            break;

        case AST_IF_STMT: {
            es_walk_expr(ctx, node->as.if_stmt.condition);
            ErrorSetBranchWalkState *walk =
                (ErrorSetBranchWalkState *) xr_calloc(1, sizeof(ErrorSetBranchWalkState));
            if (!walk) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                break;
            }
            bool merge_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            if (merge_catch_aliases)
                capture_catch_alias_state(ctx, &walk->base_catch_aliases);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->then_catch_aliases);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->else_catch_aliases);
                ctx->function_value_control_depth--;
            } else {
                capture_function_value_alias_state(ctx, &walk->base_function_values);

                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->then_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->then_function_values);

                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                if (node->as.if_stmt.else_branch)
                    es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->else_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->else_function_values);

                merge_function_value_if_states(ctx, &walk->base_function_values,
                                               &walk->then_function_values,
                                               &walk->else_function_values);
            }
            if (merge_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &walk->then_catch_aliases,
                                                      &walk->else_catch_aliases);
            else
                ctx->current_catch_alias_control_depth--;
            xr_free(walk);
            break;
        }

        case AST_WHILE_STMT: {
            es_walk_expr(ctx, node->as.while_stmt.condition);
            ErrorSetLoopWalkState *walk =
                (ErrorSetLoopWalkState *) xr_calloc(1, sizeof(ErrorSetLoopWalkState));
            if (!walk) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                break;
            }
            bool merge_while_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            if (merge_while_catch_aliases)
                capture_catch_alias_state(ctx, &walk->base_catch_aliases);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                ctx->function_value_control_depth--;
            } else {
                capture_function_value_alias_state(ctx, &walk->base_function_values);

                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->iteration_function_values);

                merge_function_value_loop_state(ctx, &walk->base_function_values,
                                                &walk->iteration_function_values);
            }
            if (merge_while_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &walk->base_catch_aliases,
                                                      &walk->iteration_catch_aliases);
            else
                ctx->current_catch_alias_control_depth--;
            xr_free(walk);
            break;
        }

        case AST_FOR_STMT: {
            es_walk_stmt(ctx, node->as.for_stmt.initializer);
            es_walk_expr(ctx, node->as.for_stmt.condition);
            ErrorSetLoopWalkState *walk =
                (ErrorSetLoopWalkState *) xr_calloc(1, sizeof(ErrorSetLoopWalkState));
            if (!walk) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                break;
            }
            bool merge_for_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            if (merge_for_catch_aliases)
                capture_catch_alias_state(ctx, &walk->base_catch_aliases);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                ctx->function_value_control_depth--;
            } else {
                capture_function_value_alias_state(ctx, &walk->base_function_values);

                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->iteration_function_values);

                merge_function_value_loop_state(ctx, &walk->base_function_values,
                                                &walk->iteration_function_values);
            }
            if (merge_for_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &walk->base_catch_aliases,
                                                      &walk->iteration_catch_aliases);
            else
                ctx->current_catch_alias_control_depth--;
            xr_free(walk);
            break;
        }

        case AST_FOR_IN_STMT: {
            es_walk_expr(ctx, node->as.for_in_stmt.collection);
            if (for_in_collection_is_known_empty_set(ctx, &node->as.for_in_stmt))
                break;
            ErrorSetLoopWalkState *walk =
                (ErrorSetLoopWalkState *) xr_calloc(1, sizeof(ErrorSetLoopWalkState));
            if (!walk) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
                break;
            }
            bool merge_for_in_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            if (merge_for_in_catch_aliases)
                capture_catch_alias_state(ctx, &walk->base_catch_aliases);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                maybe_add_for_in_catch_alias(ctx, &node->as.for_in_stmt);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                ctx->function_value_control_depth--;
            } else {
                capture_function_value_alias_state(ctx, &walk->base_function_values);

                restore_function_value_alias_state(ctx, &walk->base_function_values);
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &walk->base_catch_aliases);
                maybe_add_for_in_catch_alias(ctx, &node->as.for_in_stmt);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &walk->iteration_catch_aliases);
                capture_function_value_alias_state(ctx, &walk->iteration_function_values);

                merge_function_value_loop_state(ctx, &walk->base_function_values,
                                                &walk->iteration_function_values);
            }
            if (merge_for_in_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &walk->base_catch_aliases,
                                                      &walk->iteration_catch_aliases);
            else
                ctx->current_catch_alias_control_depth--;
            xr_free(walk);
            break;
        }

        case AST_BLOCK:
            es_walk_block(ctx, node);
            break;

        case AST_DEFER_STMT:
            /* Cleanup errors are validated in an isolated summary after the
             * ordinary function fixpoint. They do not become errors of the
             * function that registered the cleanup. */
            break;

        case AST_SCOPE_BLOCK: {
            /* A `linked scope` re-raises its first failed child at the block's
             * exit, in this frame, on the channel the child raised it on.  Walk
             * the body with that depth marked so every `go` inside it charges
             * the coroutine body's errors to this function.  A plain `scope` is
             * only a wait barrier and adds nothing. */
            bool linked = node->as.scope_block.scope_mode == XR_SCOPE_LINKED;
            if (linked)
                ctx->linked_scope_depth++;
            es_walk_block(ctx, node->as.scope_block.body);
            if (linked)
                ctx->linked_scope_depth--;
            break;
        }

        case AST_FUNCTION_DECL:
            /* Nested function: analyzed separately, skip */
            break;

        default:
            break;
    }
}

/* ========== Per-Function Inference ========== */

static void infer_function_error_set(ErrorSetCtx *ctx, AstNode *func_node, XaSymbol *func_sym) {
    if (!func_node || !func_sym)
        return;

    AstNode *body = function_like_body(func_node);
    if (!body)
        return;

    ctx->current_func = func_sym;
    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, func_node);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ctx->current_summary = &summary;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    ctx->task_spawn_alias_count = 0;
    ctx->linked_scope_depth = 0;
    clear_function_value_param_aliases(ctx, func_node);
    apply_specialized_function_param_targets(ctx, func_node, func_sym);
    es_walk_block(ctx, body);
    if (function_has_error_diagnostic(ctx, func_node, func_sym))
        xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_INVALID_PROGRAM);
    XaEffectId previous_id = func_sym->links.effect_id;
    func_sym->links.effect_id = xa_effect_db_intern(ctx->analyzer->effect_db, &summary);
    for (int i = 0; i < func_sym->links.param_effect_count; i++)
        func_sym->links.param_effects[i].callable_effects = func_sym->links.effect_id;
    if (func_sym->links.effect_id != previous_id)
        ctx->changed = true;
    store_function_return_target(ctx, func_sym);
    xa_effect_summary_clear(&summary);

    ctx->current_summary = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    ctx->analyzer->current_scope = saved_scope;
    ctx->current_func = NULL;
}

/* ========== Top-Level Collector ========== */

typedef struct FuncEntry {
    AstNode *node;
    XaSymbol *sym;
} FuncEntry;

typedef struct FunctionExprList {
    AstNode **items;
    int count;
    int capacity;
} FunctionExprList;

static void collect_function_expr_pre(AstNode *node, void *userdata) {
    FunctionExprList *list = (FunctionExprList *) userdata;
    if (!list || !node || node->type != AST_FUNCTION_EXPR)
        return;
    if (list->count >= list->capacity) {
        int next_capacity = list->capacity ? list->capacity * 2 : 16;
        XR_REALLOC_OR_ABORT(list->items, (size_t) next_capacity * sizeof(AstNode *),
                            "function expression list grow");
        list->capacity = next_capacity;
    }
    list->items[list->count++] = node;
}

/* Function expressions do not own a declaration symbol/effect-id, but their
 * function value still carries the task-216 bit. Infer the same complete/empty
 * conclusion after named-function fixpoint and publish it on the expression's
 * analyzed type. Stored and passed lambdas can then satisfy inferred callable
 * constraints without relying on syntax. */
/* Walk an anonymous function body into `out`, which the caller must have
 * initialized and must clear. Shared by throw-effect publication and by the
 * defer rule (spec 8.3.1 D1), which needs the escaping error set itself rather
 * than the collapsed no-throw bit. */
static bool compute_function_expr_summary(ErrorSetCtx *ctx, AstNode *node, XaEffectSummary *out) {
    if (!ctx || !node || !out || node->type != AST_FUNCTION_EXPR)
        return false;
    AstNode *body = function_like_body(node);
    if (!body)
        return false;
    ErrorSetAliasSnapshot *snapshot =
        (ErrorSetAliasSnapshot *) xr_calloc(1, sizeof(ErrorSetAliasSnapshot));
    if (!snapshot) {
        xa_effect_summary_mark_incomplete(out, XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
        return true;
    }

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, node);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaEffectSummary *saved_summary = ctx->current_summary;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;
    capture_function_value_alias_state(ctx, &snapshot->function_values);
    capture_catch_alias_state(ctx, &snapshot->catch_aliases);
    const char *saved_catch_var = ctx->current_catch_var;
    uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
    XaEffectSummary *saved_caught = ctx->current_caught;
    int saved_catch_alias_control_depth = ctx->current_catch_alias_control_depth;

    ctx->current_summary = out;
    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    apply_function_expr_capture(ctx, node);
    apply_function_expr_catch_capture(ctx, node);
    es_walk_block(ctx, body);

    ctx->current_summary = saved_summary;
    ctx->current_func = saved_func;
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    restore_function_value_alias_state(ctx, &snapshot->function_values);
    ctx->current_catch_var = saved_catch_var;
    ctx->current_catch_symbol_id = saved_catch_symbol_id;
    restore_catch_alias_state(ctx, &snapshot->catch_aliases);
    ctx->current_caught = saved_caught;
    ctx->current_catch_alias_control_depth = saved_catch_alias_control_depth;
    ctx->analyzer->current_scope = saved_scope;
    xr_free(snapshot);
    return true;
}

static bool compute_defer_block_summary(ErrorSetCtx *ctx, AstNode *body, XaEffectSummary *out) {
    if (!ctx || !body || !out || body->type != AST_BLOCK)
        return false;
    ErrorSetAliasSnapshot *snapshot =
        (ErrorSetAliasSnapshot *) xr_calloc(1, sizeof(ErrorSetAliasSnapshot));
    if (!snapshot) {
        xa_effect_summary_mark_incomplete(out, XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
        return true;
    }

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *body_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, body);
    if (body_scope)
        ctx->analyzer->current_scope = body_scope;
    XaEffectSummary *saved_summary = ctx->current_summary;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;
    capture_function_value_alias_state(ctx, &snapshot->function_values);
    capture_catch_alias_state(ctx, &snapshot->catch_aliases);
    CoroBoundaryState saved_coro_state;
    enter_callee_body_coro_boundary(ctx, &saved_coro_state);

    ctx->current_summary = out;
    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    es_walk_block(ctx, body);

    ctx->current_summary = saved_summary;
    ctx->current_func = saved_func;
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    restore_function_value_alias_state(ctx, &snapshot->function_values);
    restore_catch_alias_state(ctx, &snapshot->catch_aliases);
    leave_callee_body_coro_boundary(ctx, &saved_coro_state);
    ctx->analyzer->current_scope = saved_scope;
    xr_free(snapshot);
    return true;
}

static void infer_function_expr_throw_effect(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_FUNCTION_EXPR)
        return;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, node);
    if (!type || type->kind != XR_KIND_FUNCTION)
        return;

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    if (compute_function_expr_summary(ctx, node, &summary)) {
        XrFnThrowEffect effect =
            xa_effect_summary_is_nothrow(&summary) ? XR_FN_EFFECT_NO_THROW : XR_FN_EFFECT_MAY_THROW;
        xr_type_function_set_throw_effect(type, effect);
    }
    xa_effect_summary_clear(&summary);
}

static void collect_functions(XaAnalyzer *analyzer, AstNode *node, FuncEntry **out, int *count,
                              int *cap) {
    if (!node)
        return;

    if (node->type == AST_FUNCTION_DECL || node->type == AST_METHOD_DECL) {
        XaSymbol *sym = resolve_func_symbol(analyzer, node);
        /* Rejected duplicate declarations resolve to the first symbol through
         * the by-name fallback. Two bodies feeding one summary would make the
         * fixpoint oscillate forever, so only the first body per symbol is
         * inferred; the redefinition diagnostic already rejects the program. */
        for (int k = 0; sym && k < *count; k++) {
            if ((*out)[k].sym == sym) {
                sym = NULL;
                break;
            }
        }
        if (sym) {
            if (*count >= *cap) {
                int new_cap = *cap == 0 ? 32 : *cap * 2;
                FuncEntry *new_arr = (FuncEntry *) xr_malloc((size_t) new_cap * sizeof(FuncEntry));
                if (*out && *count > 0)
                    memcpy(new_arr, *out, (size_t) (*count) * sizeof(FuncEntry));
                xr_free(*out);
                *out = new_arr;
                *cap = new_cap;
            }
            (*out)[*count].node = node;
            (*out)[*count].sym = sym;
            (*count)++;
        }
    }

    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++)
            collect_functions(analyzer, node->as.program.statements[i], out, count, cap);
    }

    if (node->type == AST_CLASS_DECL) {
        for (int i = 0; i < node->as.class_decl.method_count; i++)
            collect_functions(analyzer, node->as.class_decl.methods[i], out, count, cap);
    }

    if (node->type == AST_STRUCT_DECL) {
        for (int i = 0; i < node->as.struct_decl.method_count; i++)
            collect_functions(analyzer, node->as.struct_decl.methods[i], out, count, cap);
    }
}

static XrFnThrowEffect function_value_target_throw_effect(ErrorSetCtx *ctx,
                                                          FunctionValueTarget target) {
    if (!ctx || !function_value_target_is_exact(target))
        return XR_FN_EFFECT_MAY_THROW;
    for (int i = 0; i < target.target_count; i++) {
        XaSymbol *symbol = target.target_symbols[i];
        AstNode *function_expr = target.target_function_exprs[i];
        if (symbol) {
            if (symbol->links.throw_effect != XR_FN_EFFECT_NO_THROW)
                return XR_FN_EFFECT_MAY_THROW;
            continue;
        }
        XrType *type =
            function_expr ? xa_analyzer_get_node_type(ctx->analyzer, function_expr) : NULL;
        if (!type || type->kind != XR_KIND_FUNCTION ||
            type->function.throw_effect != XR_FN_EFFECT_NO_THROW)
            return XR_FN_EFFECT_MAY_THROW;
    }
    return XR_FN_EFFECT_NO_THROW;
}

/* Materialize the effect argument of a monomorphized HOF parameter after all
 * concrete callees have their final bit. A merged/unknown target is MAY_THROW;
 * the lowering therefore skips ERR_CHECK only for a closed all-NO target set. */
static void publish_specialized_param_throw_effects(ErrorSetCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->specialized_param_target_count; i++) {
        SpecializedParamTargetEntry *entry = &ctx->specialized_param_targets[i];
        XaSymbol *func_sym = lookup_symbol_by_id(ctx, entry->function_id);
        XaSymbol *param_sym = lookup_symbol_by_id(ctx, entry->param_id);
        if (!func_sym || !param_sym || !symbol_has_function_type(param_sym))
            continue;
        XrFnThrowEffect effect = entry->unknown
                                     ? XR_FN_EFFECT_MAY_THROW
                                     : function_value_target_throw_effect(ctx, entry->target);
        xr_type_function_set_throw_effect(param_sym->links.type, effect);

        AstNode *fn_node = func_sym->links.function_decl_node;
        XrType *fn_type = func_sym->links.type;
        if (!fn_node || !fn_type || fn_type->kind != XR_KIND_FUNCTION)
            continue;
        int param_count = function_like_param_count(fn_node);
        XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);
        int limit = param_count < fn_type->function.param_count ? param_count
                                                                : fn_type->function.param_count;
        for (int p = 0; p < limit; p++) {
            XaSymbol *candidate = function_like_param_symbol(ctx, fn_node, fn_scope, p);
            if (!candidate || candidate->id != param_sym->id)
                continue;
            XrType *slot_type = xr_type_function_param_type(fn_type, p);
            xr_type_function_set_throw_effect(slot_type, effect);
            break;
        }
    }
}

static AstNode *no_throw_call_target_decl(ErrorSetCtx *ctx, AstNode *callee) {
    callee = identity_source(callee);
    if (!ctx || !callee)
        return NULL;
    if (callee->type == AST_FUNCTION_EXPR)
        return callee;
    XaSymbol *symbol = NULL;
    if (callee->type == AST_VARIABLE) {
        symbol = lookup_variable_symbol(ctx->analyzer, callee);
    } else if (callee->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection = xa_analyzer_get_selection(ctx->analyzer, callee);
        symbol = selection ? selection->target_symbol : NULL;
    }
    return symbol ? symbol->links.function_decl_node : NULL;
}

static XrParamNode **no_throw_decl_params(AstNode *node, int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        if (out_count)
            *out_count = node->as.function_decl.param_count;
        return node->as.function_decl.params;
    }
    if (node->type == AST_METHOD_DECL) {
        if (out_count)
            *out_count = node->as.method_decl.param_count;
        return node->as.method_decl.params;
    }
    return NULL;
}

static const char *no_throw_argument_name(ErrorSetCtx *ctx, AstNode *expr) {
    expr = identity_source(expr);
    if (!expr)
        return "callback";
    if (expr->type == AST_FUNCTION_EXPR)
        return expr->as.function_expr.name ? expr->as.function_expr.name : "<anonymous>";
    if (expr->type == AST_VARIABLE) {
        XaSymbol *symbol = lookup_variable_symbol(ctx->analyzer, expr);
        return symbol && symbol->name
                   ? symbol->name
                   : (expr->as.variable.name ? expr->as.variable.name : "callback");
    }
    if (expr->type == AST_MEMBER_ACCESS && expr->as.member_access.name)
        return expr->as.member_access.name;
    return "callback";
}

static bool no_throw_expr_is_proven(ErrorSetCtx *ctx, AstNode *expr) {
    if (!ctx || !expr)
        return false;
    FunctionValueTarget target = resolve_function_value_expr_target(ctx, expr, 0);
    if (function_value_target_is_exact(target))
        return function_value_target_throw_effect(ctx, target) == XR_FN_EFFECT_NO_THROW;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, identity_source(expr));
    return type && type->kind == XR_KIND_FUNCTION &&
           type->function.throw_effect == XR_FN_EFFECT_NO_THROW;
}

static bool no_throw_tref_requires_constraint(const XrTypeRef *tref) {
    return tref && tref->kind == XR_TREF_FUNCTION && tref->requires_nothrow;
}

static void report_no_throw_value_constraint(ErrorSetCtx *ctx, AstNode *site, const char *slot_name,
                                             AstNode *value) {
    if (!ctx || !site || !value || no_throw_expr_is_proven(ctx, value))
        return;
    const char *argument_name = no_throw_argument_name(ctx, value);
    char message[512];
    snprintf(message, sizeof(message),
             "nothrow callable constraint '%s' rejects value '%s': it may throw or cannot be "
             "proven non-throwing",
             slot_name ? slot_name : "callback", argument_name);
    XrLocation location = {.file = ctx->analyzer->current_file,
                           .line = (uint32_t) value->line,
                           .column = (uint32_t) value->column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                               message, &location);
}

static void no_throw_constraint_scan_pre(AstNode *node, void *userdata) {
    ErrorSetCtx *ctx = (ErrorSetCtx *) userdata;
    if (!ctx || !node)
        return;
    if (node->type == AST_CALL_EXPR) {
        AstNode *decl = no_throw_call_target_decl(ctx, node->as.call_expr.callee);
        int param_count = 0;
        XrParamNode **params = no_throw_decl_params(decl, &param_count);
        int limit =
            node->as.call_expr.arg_count < param_count ? node->as.call_expr.arg_count : param_count;
        for (int i = 0; params && i < limit; i++) {
            XrParamNode *param = params[i];
            if (!param || !no_throw_tref_requires_constraint(param->type))
                continue;
            report_no_throw_value_constraint(ctx, node, param->name,
                                             node->as.call_expr.arguments[i]);
        }
        return;
    }
    if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
        VarDeclNode *decl = &node->as.var_decl;
        if (decl->initializer && no_throw_tref_requires_constraint(decl->type_annotation))
            report_no_throw_value_constraint(ctx, node, decl->name, decl->initializer);
        return;
    }
    if (node->type == AST_ASSIGNMENT) {
        AssignmentNode *assignment = &node->as.assignment;
        XaSymbol *target = lookup_symbol_by_id(ctx, assignment->symbol_id);
        XrType *target_type = target ? target->links.type : NULL;
        if (assignment->value && target_type && target_type->kind == XR_KIND_FUNCTION &&
            target_type->function.throw_effect == XR_FN_EFFECT_NO_THROW)
            report_no_throw_value_constraint(ctx, node, target->name, assignment->value);
    }
}

static void validate_no_throw_value_constraints(ErrorSetCtx *ctx, AstNode *ast) {
    if (ctx && ast)
        xa_ast_walk(ast, no_throw_constraint_scan_pre, NULL, ctx);
}

/* ---- defer must not throw (spec §8.3.1 rule D1) ---- */

/* Does the cleanup block demonstrably throw -- i.e. did inference land on a
 * non-empty escaping error set, not merely fail to prove emptiness?
 *
 * The distinction is the whole design of spec 8.3.1. Rule D1 is deliberately
 * NOT fail-closed here, unlike the throw-effect bit in 8.0: xray has no
 * user-writable no-throw annotation, so a fail-closed defer rule would reject
 * correct cleanup code (an indirect call, a higher-order intrinsic, any member
 * whose native contract is still unwritten) with no way for the author to
 * discharge the obligation. Rule D3's runtime backstop is what covers the
 * unproven remainder, and keeping it live is what makes the layering honest.
 * If a user-facing no-throw annotation is ever added, D1 can tighten to
 * "must be proven" and D3 becomes unreachable-by-construction. */
static bool cleanup_block_definitely_throws(ErrorSetCtx *ctx, AstNode *body) {
    body = identity_source(body);
    if (!ctx || !body || body->type != AST_BLOCK)
        return false;

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    bool throws = compute_defer_block_summary(ctx, body, &summary) && summary.escaping.count > 0;
    xa_effect_summary_clear(&summary);
    return throws;
}

/* A `defer` is a resource-cleanup edge, not an error-propagation edge: cleanup
 * that fails leaves the resource in an unknown state, so the language neither
 * replaces the in-flight error (the Go model) nor swallows the cleanup error.
 *
 * The language has one form, `defer { block }`. Its escaping error set must be
 * empty; errors may be absorbed by local try/catch inside the block. The
 * runtime backstop for a target that static analysis cannot resolve is
 * XR_ERR_DEFER_THROW; see spec 8.3.1 rule D3. */
static void defer_no_throw_scan_pre(AstNode *node, void *userdata) {
    ErrorSetCtx *ctx = (ErrorSetCtx *) userdata;
    if (!ctx || !node || node->type != AST_DEFER_STMT)
        return;
    AstNode *body = node->as.defer_stmt.body;
    if (!body || !cleanup_block_definitely_throws(ctx, body))
        return;

    const char *message =
        "a defer cleanup block has an escaping error; a defer body must not let errors "
        "escape. Absorb it inside the defer: "
        "defer { try { ... } catch (e) { ... } }";
    XrLocation location = {.file = ctx->analyzer->current_file,
                           .line = (uint32_t) node->line,
                           .column = (uint32_t) node->column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_DEFER_MAY_THROW,
                               message, &location);
}

static void validate_defer_no_throw(ErrorSetCtx *ctx, AstNode *ast) {
    if (ctx && ast)
        xa_ast_walk(ast, defer_no_throw_scan_pre, NULL, ctx);
}

/* ========== Public Entry Point ========== */

void xa_infer_error_sets(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast)
        return;

    ErrorSetCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.analyzer = analyzer;

    /* Phase 1: Collect all function declarations */
    FuncEntry *funcs = NULL;
    int func_count = 0, func_cap = 0;
    collect_functions(analyzer, ast, &funcs, &func_count, &func_cap);

    FunctionExprList function_exprs = {0};
    xa_ast_walk(ast, collect_function_expr_pre, NULL, &function_exprs);

    if (func_count == 0 && function_exprs.count == 0)
        goto cleanup;

    /* Phase 2: Iterate the finite monotone effect/target domains to a real fixed point.
     * Do not stop after a guessed round count: an unfinished recursive component must
     * never be published as complete. */
    for (;;) {
        ctx.changed = false;

        for (int i = 0; i < func_count; i++) {
            infer_function_error_set(&ctx, funcs[i].node, funcs[i].sym);
        }

        if (!ctx.changed)
            break;
    }

    /* Phase 3: publish the typed throw-effect bit (task 216). After the fixpoint
     * each function symbol's interned summary is authoritative. Derive the bit
     * fail-closed — NO_THROW only when the summary is complete AND its escaping
     * error set is empty — and mirror it onto both the symbol and its function
     * type so IR lowering can decide error-check emission constructively by
     * callee effect. The error *set* stays in the effect DB and never enters the
     * type. */
    for (int i = 0; i < func_count; i++) {
        XaSymbol *sym = funcs[i].sym;
        if (!sym)
            continue;
        const XaEffectSummary *summary =
            xa_effect_db_get(analyzer->effect_db, sym->links.effect_id);
        XrFnThrowEffect effect =
            xa_effect_summary_is_nothrow(summary) ? XR_FN_EFFECT_NO_THROW : XR_FN_EFFECT_MAY_THROW;
        sym->links.throw_effect = effect;
        if (sym->links.type && sym->links.type->kind == XR_KIND_FUNCTION)
            xr_type_function_set_throw_effect(sym->links.type, effect);
    }

    /* Anonymous function values carry the same bit even though they do not
     * have an effect-id-bearing declaration symbol. Named callees are already
     * stable, so one expression pass is sufficient. */
    for (int i = 0; i < function_exprs.count; i++)
        infer_function_expr_throw_effect(&ctx, function_exprs.items[i]);

    publish_specialized_param_throw_effects(&ctx);
    validate_no_throw_value_constraints(&ctx, ast);
    validate_defer_no_throw(&ctx, ast);

cleanup:
    xr_free(function_exprs.items);
    xr_free(funcs);
    xr_free(ctx.function_return_targets);
    xr_free(ctx.specialized_param_targets);
    clear_function_expr_captures(&ctx);
}
