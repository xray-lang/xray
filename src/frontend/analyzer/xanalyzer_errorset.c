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
#include "xanalyzer_visitor.h"
#include "xa_effect_db.h"
#include "xa_selection.h"
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
    CATCH_AGGREGATE_INDEX,
    CATCH_AGGREGATE_FIELD,
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
    int callsite_inline_depth;
    FunctionValueTarget current_return_target;
    bool current_return_target_seen;
    bool current_return_target_unknown;
    XaEffectSummary *current_caught; /* Effect subset caught by current catch clause */
    XaSymbol *current_func;          /* Current function symbol */
    bool changed;                    /* Fixpoint: did anything change this iteration? */
};

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
        a->unknown_reasons != b->unknown_reasons || a->escaping.count != b->escaping.count ||
        a->root_count != b->root_count)
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
    return callee_type && XR_TYPE_IS_FUNCTION(callee_type);
}

static bool es_walk_function_expr_body(ErrorSetCtx *ctx, AstNode *function_expr) {
    if (!ctx || !function_expr || function_expr->type != AST_FUNCTION_EXPR)
        return false;
    FunctionDeclNode *fn = &function_expr->as.function_expr;
    if (!fn->body)
        return true;

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, function_expr);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;
    FunctionValueAliasState saved_alias_state;
    capture_function_value_alias_state(ctx, &saved_alias_state);
    apply_function_expr_capture(ctx, function_expr);
    const char *saved_catch_var = ctx->current_catch_var;
    uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
    CatchAliasState saved_catch_alias_state;
    capture_catch_alias_state(ctx, &saved_catch_alias_state);
    XaEffectSummary *saved_caught = ctx->current_caught;
    int saved_catch_alias_control_depth = ctx->current_catch_alias_control_depth;
    apply_function_expr_catch_capture(ctx, function_expr);
    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    es_walk_block(ctx, fn->body);
    ctx->current_catch_var = saved_catch_var;
    ctx->current_catch_symbol_id = saved_catch_symbol_id;
    restore_catch_alias_state(ctx, &saved_catch_alias_state);
    ctx->current_caught = saved_caught;
    ctx->current_catch_alias_control_depth = saved_catch_alias_control_depth;
    restore_function_value_alias_state(ctx, &saved_alias_state);
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    ctx->current_func = saved_func;
    ctx->analyzer->current_scope = saved_scope;
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
    return 0;
}

static XaSymbol *function_like_param_symbol(ErrorSetCtx *ctx, AstNode *node, XaScope *fn_scope,
                                            int index) {
    if (!ctx || !node || index < 0)
        return NULL;
    if (node->type == AST_FUNCTION_DECL) {
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

    FunctionValueAliasState saved_alias_state;
    capture_function_value_alias_state(ctx, &saved_alias_state);

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
        restore_function_value_alias_state(ctx, &saved_alias_state);
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
    ctx->callsite_inline_depth++;
    es_walk_block(ctx, body);
    ctx->callsite_inline_depth--;

    restore_function_value_alias_state(ctx, &saved_alias_state);
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
    if (a->kind == CATCH_AGGREGATE_INDEX) {
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
    if (!ctx || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
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
                if ((initializer->as.object_literal.computed &&
                     initializer->as.object_literal.computed[i]) ||
                    !key || key->type != AST_LITERAL_STRING)
                    continue;
                const char *field = key->as.literal.raw_value.string_val;
                if (field && is_current_caught_ref(ctx, initializer->as.object_literal.values[i]))
                    add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_FIELD,
                                                      -1, 0, NULL, NULL, field);
            }
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < initializer->as.map_literal.count; i++) {
                int64_t index = -1;
                uint32_t index_symbol_id = 0;
                const char *index_symbol_name = NULL;
                const char *index_string = NULL;
                if (!is_current_caught_ref(ctx, initializer->as.map_literal.values[i]) ||
                    !catch_aggregate_index_key(ctx, initializer->as.map_literal.keys[i], &index,
                                               &index_symbol_id, &index_symbol_name, &index_string))
                    continue;
                add_current_catch_aggregate_alias(ctx, symbol_id, name, CATCH_AGGREGATE_INDEX,
                                                  index, index_symbol_id, index_symbol_name,
                                                  index_string, NULL);
            }
            break;

        default:
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
    int64_t index = -1;
    uint32_t index_symbol_id = 0;
    const char *index_symbol_name = NULL;
    const char *index_string = NULL;
    bool rhs_is_caught = is_current_caught_ref(ctx, set->value);
    bool has_precise_index = catch_aggregate_index_key(ctx, set->index, &index, &index_symbol_id,
                                                       &index_symbol_name, &index_string);
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

/* ========== Expression Walking ========== */

static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;

    switch (node->type) {
        case AST_CALL_EXPR: {
            /* Walk arguments first */
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                es_walk_expr(ctx, node->as.call_expr.arguments[i]);
                invalidate_catch_aggregate_container_ref(ctx, node->as.call_expr.arguments[i]);
            }
            es_walk_expr(ctx, node->as.call_expr.callee);
            AstNode *callee_source = identity_source(node->as.call_expr.callee);
            if (callee_source && callee_source->type == AST_MEMBER_ACCESS)
                invalidate_catch_aggregate_container_ref(ctx,
                                                         callee_source->as.member_access.object);

            if (es_walk_immediate_function_expr_call(ctx, node->as.call_expr.callee))
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
            for (int i = 0; i < node->as.object_literal.count; i++) {
                if (node->as.object_literal.computed && node->as.object_literal.computed[i])
                    es_walk_expr(ctx, node->as.object_literal.keys[i]);
                es_walk_expr(ctx, node->as.object_literal.values[i]);
            }
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

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_catch_aggregate_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
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

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;

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

            XaEffectSummary try_summary;
            xa_effect_summary_init(&try_summary);
            XaEffectSummary *outer_summary = ctx->current_summary;
            bool exact_function_values = ctx->function_value_control_depth == 0;
            FunctionValueAliasState base_state;
            FunctionValueAliasState try_state;
            FunctionValueAliasState *catch_states = NULL;
            uint32_t try_mutation_ids[128];
            int try_mutation_count = 0;
            bool exact_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState catch_alias_base_state;
            CatchAliasState catch_alias_try_state;
            CatchAliasState *catch_alias_catch_states = NULL;
            bool *catch_alias_catch_reachable = NULL;

            ctx->current_summary = &try_summary;
            if (exact_catch_aliases)
                capture_catch_alias_state(ctx, &catch_alias_base_state);
            if (exact_function_values) {
                int mutation_start = ctx->function_value_mutation_count;
                int mutation_depth_before = ctx->function_value_mutation_depth;

                capture_function_value_alias_state(ctx, &base_state);
                restore_function_value_alias_state(ctx, &base_state);
                if (exact_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_alias_base_state);
                ctx->function_value_mutation_depth++;
                es_walk_block(ctx, tc->try_body);
                ctx->function_value_mutation_depth--;
                if (exact_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_alias_try_state);
                capture_function_value_alias_state(ctx, &try_state);

                for (int i = mutation_start; i < ctx->function_value_mutation_count; i++) {
                    uint32_t id = ctx->function_value_mutation_ids[i];
                    if (id == 0)
                        continue;
                    bool seen = false;
                    for (int j = 0; j < try_mutation_count; j++) {
                        if (try_mutation_ids[j] == id) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen && try_mutation_count < 128)
                        try_mutation_ids[try_mutation_count++] = id;
                }
                if (mutation_depth_before == 0)
                    ctx->function_value_mutation_count = mutation_start;

                if (tc->catch_count > 0)
                    catch_states = (FunctionValueAliasState *) xr_calloc(
                        (size_t) tc->catch_count, sizeof(FunctionValueAliasState));
            } else {
                ctx->function_value_control_depth++;
                if (exact_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_alias_base_state);
                es_walk_block(ctx, tc->try_body);
                if (exact_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_alias_try_state);
                ctx->function_value_control_depth--;
            }
            ctx->current_summary = outer_summary;

            if (tc->catch_count > 0) {
                if (exact_catch_aliases) {
                    catch_alias_catch_states = (CatchAliasState *) xr_calloc(
                        (size_t) tc->catch_count, sizeof(CatchAliasState));
                    catch_alias_catch_reachable =
                        (bool *) xr_calloc((size_t) tc->catch_count, sizeof(bool));
                }
                XaEffectSummary *caught_summaries = (XaEffectSummary *) xr_calloc(
                    (size_t) tc->catch_count, sizeof(XaEffectSummary));
                if (caught_summaries) {
                    for (int i = 0; i < tc->catch_count; i++)
                        xa_effect_summary_init(&caught_summaries[i]);
                }
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (!cc || cc->is_panic)
                        continue;
                    CatchEffectPattern cep = catch_effect_pattern(ctx->analyzer, cc);
                    if (cep.catch_all) {
                        if (caught_summaries)
                            xa_effect_summary_add_summary(ctx->analyzer->effect_db,
                                                          &caught_summaries[i], &try_summary);
                        xa_effect_summary_clear_escaping(&try_summary);
                        continue;
                    }
                    if (cep.has_enum) {
                        if (caught_summaries)
                            (cep.has_variant ? xa_effect_summary_add_variant_from_summary(
                                                   ctx->analyzer->effect_db, &caught_summaries[i],
                                                   &try_summary, cep.type_id, cep.variant_id)
                                             : xa_effect_summary_add_type_from_summary(
                                                   ctx->analyzer->effect_db, &caught_summaries[i],
                                                   &try_summary, cep.type_id));
                        if (cep.has_variant)
                            xa_effect_summary_subtract_variant(ctx->analyzer->effect_db,
                                                               &try_summary, cep.type_id,
                                                               cep.variant_id);
                        else
                            xa_effect_summary_subtract_type(&try_summary, cep.type_id);
                    }
                }
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, outer_summary,
                                              &try_summary);
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
                            restore_catch_alias_state(ctx, &catch_alias_base_state);
                        const char *saved_catch_var = ctx->current_catch_var;
                        uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
                        CatchAliasState saved_catch_alias_state;
                        capture_catch_alias_state(ctx, &saved_catch_alias_state);
                        XaEffectSummary *saved_caught = ctx->current_caught;
                        ctx->current_catch_var = cc->var_name;
                        ctx->current_catch_symbol_id = cc->symbol_id;
                        ctx->current_catch_binding_is_caught = true;
                        ctx->current_catch_alias_count = 0;
                        ctx->current_caught = caught_summaries ? &caught_summaries[i] : NULL;
                        if (exact_function_values)
                            restore_function_value_alias_state_for_catch_entry(
                                ctx, &base_state, try_mutation_ids, try_mutation_count);
                        else
                            ctx->function_value_control_depth++;
                        es_walk_block(ctx, cc->body);
                        if (exact_function_values && catch_states)
                            capture_function_value_alias_state(ctx, &catch_states[i]);
                        if (!exact_function_values)
                            ctx->function_value_control_depth--;
                        ctx->current_catch_var = saved_catch_var;
                        ctx->current_catch_symbol_id = saved_catch_symbol_id;
                        restore_catch_alias_state(ctx, &saved_catch_alias_state);
                        ctx->current_caught = saved_caught;
                        if (exact_catch_aliases && catch_alias_catch_states)
                            capture_catch_alias_state(ctx, &catch_alias_catch_states[i]);
                    } else if (exact_function_values && catch_states) {
                        restore_function_value_alias_state_for_catch_entry(
                            ctx, &base_state, try_mutation_ids, try_mutation_count);
                        capture_function_value_alias_state(ctx, &catch_states[i]);
                        if (exact_catch_aliases && catch_alias_catch_states) {
                            restore_catch_alias_state(ctx, &catch_alias_base_state);
                            capture_catch_alias_state(ctx, &catch_alias_catch_states[i]);
                        }
                    } else if (exact_catch_aliases && catch_alias_catch_states) {
                        restore_catch_alias_state(ctx, &catch_alias_base_state);
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
                                              &try_summary);
            }
            if (exact_function_values) {
                if (tc->catch_count > 0 && catch_states) {
                    const FunctionValueAliasState *paths[129];
                    int path_count = 0;
                    paths[path_count++] = &try_state;
                    for (int i = 0; i < tc->catch_count && path_count < 129; i++)
                        paths[path_count++] = &catch_states[i];
                    merge_function_value_path_states(ctx, &base_state, paths, path_count);
                } else {
                    restore_function_value_alias_state(ctx, &try_state);
                }
            }
            if (exact_catch_aliases) {
                if (tc->catch_count > 0 && catch_alias_catch_states) {
                    const CatchAliasState *paths[129];
                    int path_count = 0;
                    paths[path_count++] = &catch_alias_try_state;
                    for (int i = 0; i < tc->catch_count && path_count < 129; i++) {
                        if (catch_alias_catch_reachable && !catch_alias_catch_reachable[i])
                            continue;
                        paths[path_count++] = &catch_alias_catch_states[i];
                    }
                    merge_catch_alias_path_states(ctx, paths, path_count);
                } else {
                    restore_catch_alias_state(ctx, &catch_alias_try_state);
                }
            }
            if (catch_states)
                xr_free(catch_states);
            if (catch_alias_catch_states)
                xr_free(catch_alias_catch_states);
            if (catch_alias_catch_reachable)
                xr_free(catch_alias_catch_reachable);
            xa_effect_summary_clear(&try_summary);
            break;
        }

        case AST_EXPR_STMT:
            es_walk_expr(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
            maybe_record_catch_alias(ctx, node);
            maybe_record_catch_aggregate_alias(ctx, node);
            maybe_record_function_value_var_initializer(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_CONST_DECL:
            maybe_record_catch_alias(ctx, node);
            maybe_record_catch_aggregate_alias(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_catch_aggregate_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
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

        case AST_IF_STMT:
            es_walk_expr(ctx, node->as.if_stmt.condition);
            bool merge_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState catch_base_state;
            CatchAliasState catch_then_state;
            CatchAliasState catch_else_state;
            if (merge_catch_aliases)
                capture_catch_alias_state(ctx, &catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_then_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_else_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState then_state;
                FunctionValueAliasState else_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_then_state);
                capture_function_value_alias_state(ctx, &then_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                if (node->as.if_stmt.else_branch)
                    es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_else_state);
                capture_function_value_alias_state(ctx, &else_state);

                merge_function_value_if_states(ctx, &base_state, &then_state, &else_state);
            }
            if (merge_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &catch_then_state, &catch_else_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_WHILE_STMT:
            es_walk_expr(ctx, node->as.while_stmt.condition);
            bool merge_while_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState while_catch_base_state;
            CatchAliasState while_catch_iteration_state;
            if (merge_while_catch_aliases)
                capture_catch_alias_state(ctx, &while_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &while_catch_base_state);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &while_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &while_catch_base_state);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &while_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_while_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &while_catch_base_state,
                                                      &while_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_FOR_STMT:
            es_walk_stmt(ctx, node->as.for_stmt.initializer);
            es_walk_expr(ctx, node->as.for_stmt.condition);
            bool merge_for_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState for_catch_base_state;
            CatchAliasState for_catch_iteration_state;
            if (merge_for_catch_aliases)
                capture_catch_alias_state(ctx, &for_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &for_catch_base_state);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &for_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &for_catch_base_state);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &for_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_for_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &for_catch_base_state,
                                                      &for_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_FOR_IN_STMT:
            es_walk_expr(ctx, node->as.for_in_stmt.collection);
            bool merge_for_in_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState for_in_catch_base_state;
            CatchAliasState for_in_catch_iteration_state;
            if (merge_for_in_catch_aliases)
                capture_catch_alias_state(ctx, &for_in_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &for_in_catch_base_state);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &for_in_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &for_in_catch_base_state);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &for_in_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_for_in_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &for_in_catch_base_state,
                                                      &for_in_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_BLOCK:
            es_walk_block(ctx, node);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                es_walk_expr(ctx, node->as.print_stmt.exprs[i]);
            break;

        case AST_DEFER_STMT:
            es_walk_expr(ctx, node->as.defer_stmt.expr);
            break;

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
    clear_function_value_param_aliases(ctx, func_node);
    apply_specialized_function_param_targets(ctx, func_node, func_sym);
    es_walk_block(ctx, body);
    if (function_has_error_diagnostic(ctx, func_node, func_sym))
        xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_INVALID_PROGRAM);
    XaEffectId previous_id = func_sym->links.effect_id;
    func_sym->links.effect_id = xa_effect_db_intern(ctx->analyzer->effect_db, &summary);
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

static void collect_functions(XaAnalyzer *analyzer, AstNode *node, FuncEntry **out, int *count,
                              int *cap) {
    if (!node)
        return;

    if (node->type == AST_FUNCTION_DECL || node->type == AST_METHOD_DECL) {
        XaSymbol *sym = resolve_func_symbol(analyzer, node);
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

    if (node->type == AST_EXPORT_STMT)
        collect_functions(analyzer, node->as.export_stmt.declaration, out, count, cap);
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

    if (func_count == 0)
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

cleanup:
    xr_free(funcs);
    xr_free(ctx.function_return_targets);
    xr_free(ctx.specialized_param_targets);
    clear_function_expr_captures(&ctx);
}
