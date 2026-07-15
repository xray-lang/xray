/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_stmt.c - Pass 2 statement type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for statements: variable declarations, assignments,
 *   control flow, loops, class/enum declarations, and import handling.
 */

#include "xanalyzer_visitor_internal.h"
#include "xaddressability.h"
#include "xconsteval.h"
#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../base/xhashmap.h"
#include "../../base/xstorage.h"
#include <stdint.h>
#include <string.h>

static bool xa_is_module_level_scope(const XaAnalyzer *analyzer) {
    return analyzer && analyzer->current_scope && analyzer->current_scope->kind == XA_SCOPE_GLOBAL;
}

static XrAttribute *xa_var_attr(const VarDeclNode *var, AttributeKind kind) {
    if (!var || !var->attributes)
        return NULL;
    for (int i = 0; i < var->attr_count; i++) {
        if (var->attributes[i] && var->attributes[i]->kind == kind)
            return var->attributes[i];
    }
    return NULL;
}

static bool xa_var_has_static_data_attr(const VarDeclNode *var) {
    return xa_var_attr(var, ATTR_SECTION) || xa_var_attr(var, ATTR_WEAK) ||
           xa_var_attr(var, ATTR_USED);
}

static bool xa_type_supports_mutable_static_data_object(const XrType *type) {
    if (!type)
        return false;
    if (!type->is_nullable) {
        switch (type->kind) {
            case XR_KIND_INT:
                return type->native_width == XR_NATIVE_I64;
            case XR_KIND_FLOAT:
                return type->native_width != XR_NATIVE_F32;
            case XR_KIND_BOOL:
            case XR_KIND_RUNE:
                return true;
            default:
                break;
        }
    }
    switch (type->kind) {
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return xa_type_has_fixed_layout_data_object(type);
        default:
            return false;
    }
}

static bool xa_freestanding_top_var_static_initializer_allowed(XaInferContext *ctx,
                                                               VarDeclNode *var,
                                                               XrType *declared_type);

static void xa_validate_static_data_attrs(XaInferContext *ctx, AstNode *node, VarDeclNode *var,
                                          XaSymbolLinks *links, XrType *var_type) {
    if (!ctx || !node || !var || !xa_var_has_static_data_attr(var))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    if (!xa_freestanding_profile_enabled(ctx->analyzer)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@weak/@used static data is currently only supported in freestanding profile",
            &loc);
        return;
    }
    XrAttribute *section = xa_var_attr(var, ATTR_SECTION);
    if (section && (!section->str_arg || section->str_arg[0] == '\0')) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "@section requires a non-empty section name", &loc);
        return;
    }
    if (!xa_is_module_level_scope(ctx->analyzer) ||
        (node->type != AST_CONST_DECL && node->type != AST_VAR_DECL)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "@section/@weak/@used can only annotate a module-level const "
                                   "data declaration or mutable var static object",
                                   &loc);
        return;
    }

    if (node->type == AST_CONST_DECL) {
        if (!links || !links->has_ct_value) {
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                       "@section/@weak/@used const data requires a compile-time "
                                       "initializer",
                                       &loc);
            return;
        }
        if (!xa_type_supports_const_static_data_object(var_type)) {
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                "@section/@weak/@used const data currently requires a scalar, string, "
                "fixed-array, tuple, struct, or union static object",
                &loc);
        }
        return;
    }

    if (!var->initializer ||
        !xa_freestanding_top_var_static_initializer_allowed(ctx, var, var_type)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@weak/@used mutable static data requires a compile-time "
            "static initializer",
            &loc);
        return;
    }
    if (!xa_type_supports_mutable_static_data_object(var_type)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@weak/@used mutable static data currently requires a scalar, "
            "struct, or union static object",
            &loc);
    }
}

static bool xa_freestanding_top_const_aggregate_value_allowed(const XrCtValue *value,
                                                              bool allow_string_array_elements);

static bool xa_freestanding_top_const_tuple_element_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_STRING:
            return true;
        case XR_CT_TUPLE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_aggregate_scalar_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
            return true;
        case XR_CT_FIXED_ARRAY:
        case XR_CT_STRUCT_VALUE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_fixed_array_element_allowed(const XrCtValue *value,
                                                                  bool allow_strings) {
    if (allow_strings && value && value->kind == XR_CT_STRING)
        return true;
    return xa_freestanding_top_const_aggregate_scalar_allowed(value);
}

static bool xa_freestanding_top_const_string_fixed_array_allowed_depth(const XrCtValue *value,
                                                                       int remaining_dims) {
    if (!value || remaining_dims <= 0 || value->kind != XR_CT_FIXED_ARRAY)
        return false;
    const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
    if (array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        if (array->elements[i].kind == XR_CT_STRING)
            continue;
        if (remaining_dims > 1 && xa_freestanding_top_const_string_fixed_array_allowed_depth(
                                      &array->elements[i], remaining_dims - 1))
            continue;
        return false;
    }
    return true;
}

static bool xa_freestanding_top_const_string_fixed_array_allowed(const XrCtValue *value) {
    return xa_freestanding_top_const_string_fixed_array_allowed_depth(value, 2);
}

static bool xa_freestanding_top_const_root_string_fixed_array_allowed(const XrCtValue *value) {
    if (!value || value->kind != XR_CT_FIXED_ARRAY)
        return false;
    const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
    if (array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        if (array->elements[i].kind == XR_CT_STRING)
            continue;
        if (xa_freestanding_top_const_string_fixed_array_allowed(&array->elements[i]))
            continue;
        return false;
    }
    return true;
}

static bool xa_freestanding_top_const_root_fixed_array_element_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    if (value->kind == XR_CT_STRING)
        return true;
    if (value->kind == XR_CT_TUPLE)
        return xa_freestanding_top_const_aggregate_value_allowed(value, false);
    if (xa_freestanding_top_const_string_fixed_array_allowed(value))
        return true;
    return xa_freestanding_top_const_aggregate_scalar_allowed(value);
}

static bool xa_freestanding_top_const_root_fixed_array_allowed(const XrCtValue *value) {
    if (!value || value->kind != XR_CT_FIXED_ARRAY)
        return false;
    if (xa_freestanding_top_const_root_string_fixed_array_allowed(value))
        return true;
    const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
    if (array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        if (!xa_freestanding_top_const_root_fixed_array_element_allowed(&array->elements[i]))
            return false;
    }
    return true;
}

static bool xa_freestanding_top_const_struct_field_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    if (value->kind == XR_CT_STRING)
        return true;
    if (value->kind == XR_CT_FIXED_ARRAY)
        return xa_freestanding_top_const_aggregate_value_allowed(value, true);
    return xa_freestanding_top_const_aggregate_scalar_allowed(value);
}

static bool xa_freestanding_top_const_aggregate_value_allowed(const XrCtValue *value,
                                                              bool allow_string_array_elements) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_FIXED_ARRAY: {
            const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
            if (array->count <= 0 || !array->elements)
                return false;
            for (int i = 0; i < array->count; i++) {
                if (!xa_freestanding_top_const_fixed_array_element_allowed(
                        &array->elements[i], allow_string_array_elements))
                    return false;
            }
            return true;
        }
        case XR_CT_TUPLE: {
            const XrCtTupleValue *tuple = &value->as.tuple_val;
            if (tuple->count <= 0 || !tuple->elements)
                return false;
            for (int i = 0; i < tuple->count; i++) {
                if (!xa_freestanding_top_const_tuple_element_allowed(&tuple->elements[i]))
                    return false;
            }
            return true;
        }
        case XR_CT_STRUCT_VALUE: {
            const XrCtStructValue *st = &value->as.struct_val;
            if (st->field_count <= 0 || !st->field_values)
                return false;
            for (int i = 0; i < st->field_count; i++) {
                if (!xa_freestanding_top_const_struct_field_allowed(&st->field_values[i]))
                    return false;
            }
            return true;
        }
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_ct_value_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_STRING:
        case XR_CT_NULL:
            return true;
        case XR_CT_FIXED_ARRAY:
            return xa_freestanding_top_const_root_fixed_array_allowed(value);
        case XR_CT_TUPLE:
        case XR_CT_STRUCT_VALUE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static int xa_analyzer_error_diagnostic_count(XaAnalyzer *analyzer) {
    int count = 0;
    if (!analyzer)
        return 0;
    for (XaDiagnostic *d = analyzer->diagnostics; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_ERROR)
            count++;
    }
    return count;
}

static bool xa_warning_already_reported(XaAnalyzer *analyzer, const XrLocation *loc,
                                        const char *message) {
    if (!analyzer || !loc || !message)
        return false;
    for (XaDiagnostic *d = analyzer->diagnostics; d; d = d->next) {
        if (d->severity != XR_DIAG_SEV_WARNING || d->code != XR_ERR_ANALYZE || !d->message ||
            strcmp(d->message, message) != 0)
            continue;
        if (d->location.line != loc->line || d->location.column != loc->column)
            continue;
        if (d->location.file == loc->file ||
            (d->location.file && loc->file && strcmp(d->location.file, loc->file) == 0))
            return true;
    }
    return false;
}

static bool xa_symbol_is_local_thread_handle(XaSymbol *sym, XaScope *current_scope) {
    if (!sym || sym->kind != XA_SYM_VARIABLE || !current_scope)
        return false;
    if (sym->scope != current_scope)
        return false;
    if (sym->is_shared || sym->is_exported || sym->is_imported)
        return false;
    XaSymbolLinks *links = &sym->links;
    return links->type && xr_type_is_named_class(links->type, "Thread");
}

static XrType *xa_lifecycle_lint_param_type(XaInferContext *ctx, XrParamNode *param) {
    if (!ctx || !ctx->analyzer || !param)
        return NULL;
    if (param->type)
        return xr_tref_resolve_in_analyzer(ctx->analyzer, param->type);
    return NULL;
}

static XaSymbol *xa_lifecycle_lint_function_symbol(XaInferContext *ctx,
                                                   const FunctionDeclNode *fn) {
    if (!ctx || !ctx->analyzer || !fn)
        return NULL;
    if (fn->symbol_id != 0)
        return xa_scope_lookup_by_id(ctx->analyzer->global_scope, fn->symbol_id);
    if (fn->name)
        return xa_lookup_visible_symbol(ctx, fn->name);
    return NULL;
}

static XrType *xa_lifecycle_lint_function_param_type(XaInferContext *ctx,
                                                     const FunctionDeclNode *fn, int index) {
    if (!ctx || !fn || index < 0 || index >= fn->param_count)
        return NULL;
    XaSymbol *sym = xa_lifecycle_lint_function_symbol(ctx, fn);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    if (links && links->param_types && index < links->param_count && links->param_types[index])
        return links->param_types[index];
    XrParamNode *param = fn->params ? fn->params[index] : NULL;
    return xa_lifecycle_lint_param_type(ctx, param);
}

static const char *xa_lifecycle_lint_tref_name(const XrTypeRef *tref) {
    if (!tref)
        return NULL;
    while (tref && tref->kind == XR_TREF_OPTIONAL && tref->nchildren > 0 && tref->children)
        tref = tref->children[0];
    if (!tref)
        return NULL;
    if ((tref->kind == XR_TREF_NAMED || tref->kind == XR_TREF_GENERIC) && tref->name)
        return tref->name;
    return NULL;
}

static bool xa_lifecycle_lint_function_param_type_ref_named(const FunctionDeclNode *fn, int index,
                                                            const char *name) {
    if (!fn || index < 0 || index >= fn->param_count || !name)
        return false;
    XrParamNode *param = fn->params ? fn->params[index] : NULL;
    const char *tref_name = param ? xa_lifecycle_lint_tref_name(param->type) : NULL;
    return tref_name && strcmp(tref_name, name) == 0;
}

static bool xa_lifecycle_lint_function_param_is_named_class(XaInferContext *ctx,
                                                            const FunctionDeclNode *fn, int index,
                                                            const char *class_name) {
    XrType *type = xa_lifecycle_lint_function_param_type(ctx, fn, index);
    if (type && xr_type_is_named_class(type, class_name))
        return true;
    return xa_lifecycle_lint_function_param_type_ref_named(fn, index, class_name);
}

static XaScope *xa_lifecycle_lint_current_root_scope(XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    XaScope *scope = ctx->analyzer->current_scope;
    while (scope && scope->parent)
        scope = scope->parent;
    return scope;
}

static XaSymbol *xa_lifecycle_lint_function_param_symbol(XaInferContext *ctx, AstNode *fn_node,
                                                         XrParamNode *param) {
    if (!ctx || !ctx->analyzer || !fn_node || !param || !param->name)
        return NULL;
    XaScope *root = xa_lifecycle_lint_current_root_scope(ctx);
    XaScope *fn_scope = root ? xa_scope_find_by_node(root, fn_node) : NULL;
    if (!fn_scope && ctx->analyzer->global_scope && ctx->analyzer->global_scope != root)
        fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);
    return fn_scope ? xa_scope_lookup_local(fn_scope, param->name) : NULL;
}

static bool xa_block_node_statements(AstNode *node, AstNode ***out_statements, int *out_count);
static XaScope *xa_find_enclosing_function_scope(XaInferContext *ctx);

typedef struct XaThreadHandleLintAlias {
    XaSymbol *sym;
    uint32_t symbol_id;
    const char *name;
    struct XaThreadHandleLintAlias *next;
} XaThreadHandleLintAlias;

typedef struct XaThreadHandleLintFnSummary XaThreadHandleLintFnSummary;

typedef struct XaThreadHandleLintState {
    XaInferContext *ctx;
    XaSymbol *root;
    AstNode *decl_stmt;
    XaThreadHandleLintAlias *aliases;
    XaThreadHandleLintFnSummary *fn_summaries;
    bool finalized;
    bool transferred;
    struct XaThreadHandleLintState *next;
} XaThreadHandleLintState;

typedef struct XaThreadHandleLintSnapshot {
    bool finalized;
    bool transferred;
    XaThreadHandleLintAlias *aliases;
} XaThreadHandleLintSnapshot;

struct XaThreadHandleLintFnSummary {
    const char *name;
    uint32_t symbol_id;
    bool *param_finalized;
    int return_param_index;
    bool returns_new_handle;
    int param_count;
    struct XaThreadHandleLintFnSummary *next;
};

static XaThreadHandleLintState *
xa_thread_lint_find_returned_call_arg(XaThreadHandleLintState *states, AstNode *expr);

static void xa_thread_lint_free_fn_summaries(XaThreadHandleLintFnSummary *summaries) {
    while (summaries) {
        XaThreadHandleLintFnSummary *next = summaries->next;
        xr_free(summaries->param_finalized);
        xr_free(summaries);
        summaries = next;
    }
}

static void xa_thread_lint_free_aliases(XaThreadHandleLintAlias *aliases) {
    while (aliases) {
        XaThreadHandleLintAlias *next = aliases->next;
        xr_free(aliases);
        aliases = next;
    }
}

static XaThreadHandleLintAlias *xa_thread_lint_clone_aliases(XaThreadHandleLintAlias *aliases) {
    XaThreadHandleLintAlias *head = NULL;
    XaThreadHandleLintAlias **tail = &head;
    for (XaThreadHandleLintAlias *alias = aliases; alias; alias = alias->next) {
        XaThreadHandleLintAlias *copy = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
        if (!copy)
            break;
        *copy = *alias;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

static void xa_thread_lint_free_states(XaThreadHandleLintState *states) {
    while (states) {
        XaThreadHandleLintState *next = states->next;
        xa_thread_lint_free_aliases(states->aliases);
        xr_free(states);
        states = next;
    }
}

static void xa_thread_lint_add_alias(XaThreadHandleLintState *state, XaSymbol *sym) {
    if (!state || !sym || sym->id == 0)
        return;
    uint32_t symbol_id = sym->id;
    for (XaThreadHandleLintAlias *a = state->aliases; a; a = a->next) {
        if (a->sym == sym || a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
            return;
    }
    XaThreadHandleLintAlias *alias = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!alias)
        return;
    alias->sym = sym;
    alias->symbol_id = symbol_id;
    alias->name = sym->name;
    alias->next = state->aliases;
    state->aliases = alias;
}

static void xa_thread_lint_add_alias_id(XaThreadHandleLintState *state, uint32_t symbol_id) {
    if (!state || symbol_id == 0)
        return;
    for (XaThreadHandleLintAlias *a = state->aliases; a; a = a->next) {
        if (a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
            return;
    }
    XaThreadHandleLintAlias *alias = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!alias)
        return;
    alias->symbol_id = symbol_id;
    alias->next = state->aliases;
    state->aliases = alias;
}

static void xa_thread_lint_add_alias_name(XaThreadHandleLintState *state, const char *name) {
    if (!state || !name)
        return;
    for (XaThreadHandleLintAlias *a = state->aliases; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0)
            return;
    }
    XaThreadHandleLintAlias *alias = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!alias)
        return;
    alias->name = name;
    alias->next = state->aliases;
    state->aliases = alias;
}

static bool xa_thread_lint_alias_has_symbol_id(XaThreadHandleLintAlias *alias, uint32_t symbol_id) {
    return alias && symbol_id != 0 &&
           (alias->symbol_id == symbol_id || (alias->sym && alias->sym->id == symbol_id));
}

static void xa_thread_lint_remove_alias_id(XaThreadHandleLintState *states, uint32_t symbol_id) {
    if (symbol_id == 0)
        return;
    for (XaThreadHandleLintState *s = states; s; s = s->next) {
        XaThreadHandleLintAlias **link = &s->aliases;
        while (*link) {
            XaThreadHandleLintAlias *alias = *link;
            if (xa_thread_lint_alias_has_symbol_id(alias, symbol_id)) {
                *link = alias->next;
                xr_free(alias);
                continue;
            }
            link = &alias->next;
        }
    }
}

static XaThreadHandleLintState *xa_thread_lint_find_by_symbol_id(XaThreadHandleLintState *states,
                                                                 uint32_t symbol_id) {
    if (symbol_id == 0)
        return NULL;
    for (XaThreadHandleLintState *s = states; s; s = s->next) {
        for (XaThreadHandleLintAlias *a = s->aliases; a; a = a->next) {
            if (a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
                return s;
        }
    }
    return NULL;
}

static int xa_thread_lint_state_count(XaThreadHandleLintState *states) {
    int count = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next)
        count++;
    return count;
}

static void xa_thread_lint_snapshot_states(XaThreadHandleLintState *states,
                                           XaThreadHandleLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        xa_thread_lint_free_aliases(snapshots[i].aliases);
        snapshots[i].finalized = s->finalized;
        snapshots[i].transferred = s->transferred;
        snapshots[i].aliases = xa_thread_lint_clone_aliases(s->aliases);
    }
}

static void xa_thread_lint_restore_states(XaThreadHandleLintState *states,
                                          XaThreadHandleLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        s->finalized = snapshots[i].finalized;
        s->transferred = snapshots[i].transferred;
        xa_thread_lint_free_aliases(s->aliases);
        s->aliases = xa_thread_lint_clone_aliases(snapshots[i].aliases);
    }
}

static void xa_thread_lint_clear_snapshot_array(XaThreadHandleLintSnapshot *snapshots, int count) {
    if (!snapshots)
        return;
    for (int i = 0; i < count; i++) {
        xa_thread_lint_free_aliases(snapshots[i].aliases);
        memset(&snapshots[i], 0, sizeof(snapshots[i]));
    }
}

static void xa_thread_lint_free_snapshot_array(XaThreadHandleLintSnapshot *snapshots, int count) {
    xa_thread_lint_clear_snapshot_array(snapshots, count);
    xr_free(snapshots);
}

static uint32_t xa_thread_lint_alias_key_id(XaThreadHandleLintAlias *alias) {
    if (!alias)
        return 0;
    if (alias->symbol_id != 0)
        return alias->symbol_id;
    return alias->sym ? alias->sym->id : 0;
}

static bool xa_thread_lint_alias_same_key(XaThreadHandleLintAlias *a, XaThreadHandleLintAlias *b) {
    if (!a || !b)
        return false;
    uint32_t a_id = xa_thread_lint_alias_key_id(a);
    uint32_t b_id = xa_thread_lint_alias_key_id(b);
    if (a_id != 0 || b_id != 0)
        return a_id != 0 && a_id == b_id;
    if (a->sym || b->sym)
        return a->sym && a->sym == b->sym;
    return a->name && b->name && strcmp(a->name, b->name) == 0;
}

static void xa_thread_lint_remove_alias_like(XaThreadHandleLintState *states,
                                             XaThreadHandleLintAlias *key) {
    if (!key)
        return;
    for (XaThreadHandleLintState *s = states; s; s = s->next) {
        XaThreadHandleLintAlias **link = &s->aliases;
        while (*link) {
            XaThreadHandleLintAlias *alias = *link;
            if (xa_thread_lint_alias_same_key(alias, key)) {
                *link = alias->next;
                xr_free(alias);
                continue;
            }
            link = &alias->next;
        }
    }
}

static void xa_thread_lint_add_alias_copy(XaThreadHandleLintState *state,
                                          XaThreadHandleLintAlias *source) {
    if (!state || !source)
        return;
    for (XaThreadHandleLintAlias *alias = state->aliases; alias; alias = alias->next) {
        if (xa_thread_lint_alias_same_key(alias, source))
            return;
    }
    XaThreadHandleLintAlias *copy = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!copy)
        return;
    *copy = *source;
    copy->next = state->aliases;
    state->aliases = copy;
}

static XaThreadHandleLintState *xa_thread_lint_state_at_index(XaThreadHandleLintState *states,
                                                              int index) {
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        if (i == index)
            return s;
    }
    return NULL;
}

static XaThreadHandleLintAlias *
xa_thread_lint_snapshot_find_alias(XaThreadHandleLintSnapshot *snapshots, int count,
                                   XaThreadHandleLintAlias *key, int *source_index_out) {
    if (source_index_out)
        *source_index_out = -1;
    if (!snapshots || !key)
        return NULL;
    for (int i = 0; i < count; i++) {
        for (XaThreadHandleLintAlias *alias = snapshots[i].aliases; alias; alias = alias->next) {
            if (xa_thread_lint_alias_same_key(alias, key)) {
                if (source_index_out)
                    *source_index_out = i;
                return alias;
            }
        }
    }
    return NULL;
}

static bool xa_thread_lint_alias_key_seen(XaThreadHandleLintAlias *keys,
                                          XaThreadHandleLintAlias *key) {
    for (XaThreadHandleLintAlias *seen = keys; seen; seen = seen->next) {
        if (xa_thread_lint_alias_same_key(seen, key))
            return true;
    }
    return false;
}

static void xa_thread_lint_add_alias_merge_key(XaThreadHandleLintAlias **keys,
                                               XaThreadHandleLintAlias *key) {
    if (!keys || !key || xa_thread_lint_alias_key_seen(*keys, key))
        return;
    XaThreadHandleLintAlias *copy = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!copy)
        return;
    *copy = *key;
    copy->next = *keys;
    *keys = copy;
}

static void xa_thread_lint_collect_alias_merge_keys(XaThreadHandleLintAlias **keys,
                                                    XaThreadHandleLintSnapshot *snapshots,
                                                    int count) {
    if (!keys || !snapshots)
        return;
    for (int i = 0; i < count; i++) {
        for (XaThreadHandleLintAlias *alias = snapshots[i].aliases; alias; alias = alias->next) {
            xa_thread_lint_add_alias_merge_key(keys, alias);
        }
    }
}

static void xa_thread_lint_merge_alias_snapshots(XaThreadHandleLintState *states,
                                                 XaThreadHandleLintSnapshot *before,
                                                 XaThreadHandleLintSnapshot **paths, int path_count,
                                                 int state_count) {
    if (!states || !before || !paths || path_count <= 0 || state_count <= 0)
        return;

    XaThreadHandleLintAlias *keys = NULL;
    xa_thread_lint_collect_alias_merge_keys(&keys, before, state_count);
    for (int pi = 0; pi < path_count; pi++)
        xa_thread_lint_collect_alias_merge_keys(&keys, paths[pi], state_count);

    for (XaThreadHandleLintAlias *key = keys; key; key = key->next) {
        int stable_source = -1;
        XaThreadHandleLintAlias *stable_alias = NULL;
        bool stable = true;
        for (int pi = 0; pi < path_count; pi++) {
            int source_index = -1;
            XaThreadHandleLintAlias *alias =
                xa_thread_lint_snapshot_find_alias(paths[pi], state_count, key, &source_index);
            if (!alias || source_index < 0 ||
                (stable_source >= 0 && source_index != stable_source)) {
                stable = false;
                break;
            }
            if (stable_source < 0) {
                stable_source = source_index;
                stable_alias = alias;
            }
        }

        xa_thread_lint_remove_alias_like(states, key);
        if (!stable)
            continue;
        XaThreadHandleLintState *target = xa_thread_lint_state_at_index(states, stable_source);
        xa_thread_lint_add_alias_copy(target, stable_alias ? stable_alias : key);
    }

    xa_thread_lint_free_aliases(keys);
}

static void xa_thread_lint_free_snapshot_paths(XaThreadHandleLintSnapshot **paths, int path_count,
                                               int state_count,
                                               XaThreadHandleLintSnapshot *borrowed_a,
                                               XaThreadHandleLintSnapshot *borrowed_b) {
    if (!paths)
        return;
    for (int i = 0; i < path_count; i++) {
        if (paths[i] && paths[i] != borrowed_a && paths[i] != borrowed_b)
            xa_thread_lint_free_snapshot_array(paths[i], state_count);
    }
    xr_free(paths);
}

static bool xa_thread_lint_snapshot_closed(XaThreadHandleLintSnapshot *snapshot) {
    return snapshot && (snapshot->finalized || snapshot->transferred);
}

static AstNode *xa_thread_lint_unwrap_expr(AstNode *expr) {
    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.unary.operand;
    return expr;
}

static AstNode *xa_lifecycle_lint_tail_return_expr(AstNode *body) {
    AstNode **statements = NULL;
    int count = 0;
    if (xa_block_node_statements(body, &statements, &count)) {
        if (count <= 0)
            return NULL;
        body = statements[count - 1];
    }
    if (!body || body->type != AST_RETURN_STMT)
        return NULL;
    ReturnStmtNode *ret = &body->as.return_stmt;
    return ret->value_count == 1 && ret->values ? ret->values[0] : NULL;
}

static AstNode *xa_lifecycle_lint_returned_handle_expr(AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (expr && expr->type == AST_MOVE_EXPR)
        return xa_thread_lint_unwrap_expr(expr->as.move_expr.expr);
    return expr;
}

static AstNode *xa_lifecycle_lint_single_expr_block_value(AstNode *body) {
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(body, &statements, &count) || count != 1 || !statements ||
        !statements[0] || statements[0]->type != AST_EXPR_STMT)
        return NULL;
    return statements[0]->as.expr_stmt;
}

static bool xa_lifecycle_lint_expr_known_true(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    if (expr->type == AST_LITERAL_TRUE)
        return true;
    if (!ctx || !ctx->analyzer)
        return false;
    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, expr, &value, &err))
        return false;
    return value.kind == XR_CT_BOOL && value.as.bool_val;
}

static bool xa_lifecycle_lint_break_targets_loop(AstNode *stmt, const char *loop_label) {
    if (!stmt || stmt->type != AST_BREAK_STMT)
        return false;
    const char *break_label = stmt->as.break_stmt.label;
    if (!break_label)
        return true;
    return loop_label && strcmp(break_label, loop_label) == 0;
}

static bool xa_lifecycle_lint_branch_is_loop_break(AstNode *branch, const char *loop_label) {
    if (!branch)
        return false;
    if (xa_lifecycle_lint_break_targets_loop(branch, loop_label))
        return true;
    AstNode **statements = NULL;
    int count = 0;
    if (xa_block_node_statements(branch, &statements, &count) && count == 1)
        return xa_lifecycle_lint_branch_is_loop_break(statements[0], loop_label);
    return false;
}

static bool xa_lifecycle_lint_node_skips_loop_tail_impl(AstNode *node, const char *loop_label,
                                                        bool inside_nested_loop) {
    if (!node)
        return false;

    if (node->type == AST_BREAK_STMT) {
        const char *break_label = node->as.break_stmt.label;
        if (break_label)
            return loop_label && strcmp(break_label, loop_label) == 0;
        return !inside_nested_loop;
    }
    if (node->type == AST_CONTINUE_STMT) {
        const char *continue_label = node->as.continue_stmt.label;
        if (continue_label)
            return loop_label && strcmp(continue_label, loop_label) == 0;
        return !inside_nested_loop;
    }
    if (node->type == AST_RETURN_STMT || node->type == AST_THROW_STMT)
        return true;

    AstNode **statements = NULL;
    int count = 0;
    if (xa_block_node_statements(node, &statements, &count)) {
        for (int i = 0; i < count; i++) {
            if (xa_lifecycle_lint_node_skips_loop_tail_impl(statements[i], loop_label,
                                                            inside_nested_loop))
                return true;
        }
        return false;
    }

    switch (node->type) {
        case AST_EXPR_STMT:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.expr_stmt, loop_label,
                                                               inside_nested_loop);
        case AST_IF_STMT:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.if_stmt.then_branch,
                                                               loop_label, inside_nested_loop) ||
                   xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.if_stmt.else_branch,
                                                               loop_label, inside_nested_loop);
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (xa_lifecycle_lint_node_skips_loop_tail_impl(tc->try_body, loop_label,
                                                            inside_nested_loop))
                return true;
            for (int i = 0; i < tc->catch_count; i++) {
                if (tc->catch_clauses[i] &&
                    xa_lifecycle_lint_node_skips_loop_tail_impl(tc->catch_clauses[i]->body,
                                                                loop_label, inside_nested_loop))
                    return true;
            }
            return false;
        }
        case AST_MATCH_EXPR:
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                if (xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.match_expr.arms[i],
                                                                loop_label, inside_nested_loop))
                    return true;
            }
            return false;
        case AST_MATCH_ARM:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.match_arm.body, loop_label,
                                                               inside_nested_loop);
        case AST_SELECT_STMT:
            for (int i = 0; i < node->as.select_stmt.case_count; i++) {
                if (xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.select_stmt.cases[i],
                                                                loop_label, inside_nested_loop))
                    return true;
            }
            return false;
        case AST_SELECT_CASE:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.select_case.body,
                                                               loop_label, inside_nested_loop);
        case AST_SCOPE_BLOCK:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.scope_block.body,
                                                               loop_label, inside_nested_loop);
        case AST_UNSAFE_EXPR:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.unsafe_expr.operand,
                                                               loop_label, inside_nested_loop);
        case AST_WHILE_STMT:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.while_stmt.body, loop_label,
                                                               true);
        case AST_FOR_STMT:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.for_stmt.body, loop_label,
                                                               true);
        case AST_FOR_IN_STMT:
            return xa_lifecycle_lint_node_skips_loop_tail_impl(node->as.for_in_stmt.body,
                                                               loop_label, true);
        default:
            return false;
    }
}

static bool xa_lifecycle_lint_node_skips_loop_tail(AstNode *node, const char *loop_label) {
    return xa_lifecycle_lint_node_skips_loop_tail_impl(node, loop_label, false);
}

static bool xa_lifecycle_lint_node_exits_current_scope(AstNode *node, const char *loop_label) {
    return xa_lifecycle_lint_node_skips_loop_tail(node, loop_label);
}

static bool xa_lifecycle_lint_branch_tail_breaks_loop(AstNode *branch, const char *loop_label) {
    if (!branch)
        return false;
    if (xa_lifecycle_lint_break_targets_loop(branch, loop_label))
        return true;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(branch, &statements, &count) || count <= 0)
        return false;
    for (int i = 0; i + 1 < count; i++) {
        if (xa_lifecycle_lint_node_exits_current_scope(statements[i], loop_label))
            return false;
    }
    return xa_lifecycle_lint_branch_tail_breaks_loop(statements[count - 1], loop_label);
}

static bool xa_lifecycle_lint_body_has_non_tail_exit(AstNode *body) {
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(body, &statements, &count))
        return xa_lifecycle_lint_node_exits_current_scope(body, NULL);
    for (int i = 0; i + 1 < count; i++) {
        if (xa_lifecycle_lint_node_exits_current_scope(statements[i], NULL))
            return true;
    }
    return false;
}

static bool xa_lifecycle_lint_loop_always_enters(XaInferContext *ctx, AstNode *stmt) {
    if (!stmt)
        return false;
    if (stmt->type == AST_WHILE_STMT)
        return xa_lifecycle_lint_expr_known_true(ctx, stmt->as.while_stmt.condition);
    if (stmt->type == AST_FOR_STMT)
        return !stmt->as.for_stmt.condition ||
               xa_lifecycle_lint_expr_known_true(ctx, stmt->as.for_stmt.condition);
    return false;
}

static bool xa_lifecycle_lint_const_int_expr(XaInferContext *ctx, AstNode *expr, int64_t *out) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    if (expr->type == AST_LITERAL_INT && !expr->as.literal.int_overflows_i64) {
        if (out)
            *out = expr->as.literal.raw_value.int_val;
        return true;
    }
    if (!ctx || !ctx->analyzer)
        return false;
    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, expr, &value, &err))
        return false;
    if (value.kind != XR_CT_INT)
        return false;
    if (out)
        *out = value.as.int_val;
    return true;
}

static bool xa_lifecycle_lint_positive_int_expr(XaInferContext *ctx, AstNode *expr) {
    int64_t value = 0;
    return xa_lifecycle_lint_const_int_expr(ctx, expr, &value) && value > 0;
}

static bool xa_lifecycle_lint_non_empty_range(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_RANGE)
        return false;
    int64_t start = 0;
    int64_t end = 0;
    if (!xa_lifecycle_lint_const_int_expr(ctx, expr->as.range.start, &start) ||
        !xa_lifecycle_lint_const_int_expr(ctx, expr->as.range.end, &end))
        return false;
    return expr->as.range.inclusive_end ? start <= end : start < end;
}

static bool xa_lifecycle_lint_literal_has_non_spread_element(AstNode **elements, int count) {
    if (!elements || count <= 0)
        return false;
    for (int i = 0; i < count; i++) {
        AstNode *element = xa_thread_lint_unwrap_expr(elements[i]);
        if (element && element->type != AST_SPREAD_EXPR)
            return true;
    }
    return false;
}

static bool xa_lifecycle_lint_non_empty_collection_expr(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    switch (expr->type) {
        case AST_RANGE:
            return xa_lifecycle_lint_non_empty_range(ctx, expr);
        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat)
                return xa_lifecycle_lint_positive_int_expr(ctx,
                                                           expr->as.array_literal.repeat_count);
            return xa_lifecycle_lint_literal_has_non_spread_element(expr->as.array_literal.elements,
                                                                    expr->as.array_literal.count);
        case AST_TUPLE_LITERAL:
            return xa_lifecycle_lint_literal_has_non_spread_element(expr->as.tuple_literal.elements,
                                                                    expr->as.tuple_literal.count);
        case AST_MAP_LITERAL:
            return expr->as.map_literal.count > 0;
        case AST_SET_LITERAL:
            return xa_lifecycle_lint_literal_has_non_spread_element(expr->as.set_literal.elements,
                                                                    expr->as.set_literal.count);
        case AST_LITERAL_STRING:
            return expr->as.literal.raw_value.string_val &&
                   expr->as.literal.raw_value.string_val[0] != '\0';
        default:
            return false;
    }
}

static XaSymbol *xa_lifecycle_lint_expr_const_symbol(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE)
        return NULL;
    XaSymbol *sym = NULL;
    if (expr->as.variable.symbol_id != 0)
        sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, expr->as.variable.symbol_id);
    if (!sym && expr->as.variable.name)
        sym = xa_lookup_visible_symbol(ctx, expr->as.variable.name);
    if (!sym || sym->kind != XA_SYM_VARIABLE || !sym->is_const || sym->is_rebindable ||
        sym->is_shared || sym->is_imported)
        return NULL;
    return sym;
}

static bool xa_lifecycle_lint_known_non_empty_collection(XaInferContext *ctx, AstNode *expr,
                                                         int depth) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (xa_lifecycle_lint_non_empty_collection_expr(ctx, expr))
        return true;
    if (depth <= 0)
        return false;
    XaSymbol *sym = xa_lifecycle_lint_expr_const_symbol(ctx, expr);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    if (!links || !links->const_initializer)
        return false;
    return xa_lifecycle_lint_known_non_empty_collection(ctx, links->const_initializer, depth - 1);
}

static bool xa_lifecycle_lint_for_in_always_enters(XaInferContext *ctx, AstNode *stmt) {
    return stmt && stmt->type == AST_FOR_IN_STMT &&
           xa_lifecycle_lint_known_non_empty_collection(ctx, stmt->as.for_in_stmt.collection, 4);
}

static AstNode *xa_lifecycle_lint_loop_body(AstNode *stmt) {
    if (!stmt)
        return NULL;
    if (stmt->type == AST_WHILE_STMT)
        return stmt->as.while_stmt.body;
    if (stmt->type == AST_FOR_STMT)
        return stmt->as.for_stmt.body;
    return NULL;
}

static const char *xa_lifecycle_lint_loop_label(AstNode *stmt) {
    if (!stmt)
        return NULL;
    if (stmt->type == AST_WHILE_STMT)
        return stmt->as.while_stmt.label;
    if (stmt->type == AST_FOR_STMT)
        return stmt->as.for_stmt.label;
    return NULL;
}

static uint32_t xa_thread_lint_expr_symbol_id(AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_VARIABLE)
        return 0;
    return expr->as.variable.symbol_id;
}

static const char *xa_thread_lint_expr_symbol_name(AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_VARIABLE)
        return NULL;
    return expr->as.variable.name;
}

static XaThreadHandleLintState *xa_thread_lint_find_by_expr(XaThreadHandleLintState *states,
                                                            AstNode *expr) {
    uint32_t symbol_id = xa_thread_lint_expr_symbol_id(expr);
    XaThreadHandleLintState *state = xa_thread_lint_find_by_symbol_id(states, symbol_id);
    if (state)
        return state;
    const char *name = xa_thread_lint_expr_symbol_name(expr);
    if (!name)
        return NULL;
    for (XaThreadHandleLintState *s = states; s; s = s->next) {
        if (s->root || s->decl_stmt)
            continue;
        for (XaThreadHandleLintAlias *a = s->aliases; a; a = a->next) {
            if (a->name && strcmp(a->name, name) == 0)
                return s;
        }
    }
    return NULL;
}

static uint32_t xa_lifecycle_lint_callee_symbol_id(XaInferContext *ctx, AstNode *callee) {
    callee = xa_thread_lint_unwrap_expr(callee);
    if (!callee || callee->type != AST_VARIABLE)
        return 0;
    if (callee->as.variable.symbol_id != 0)
        return callee->as.variable.symbol_id;
    XaSymbol *sym =
        callee->as.variable.name ? xa_lookup_visible_symbol(ctx, callee->as.variable.name) : NULL;
    return sym ? sym->id : 0;
}

static void xa_thread_lint_scan_expr(XaThreadHandleLintState *states, AstNode *expr,
                                     bool return_value, bool can_escape);
static void xa_thread_lint_scan_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                     bool can_escape);
static XaThreadHandleLintState *
xa_thread_lint_find_returned_call_arg(XaThreadHandleLintState *states, AstNode *expr);
static void xa_thread_lint_scan_ternary_expr(XaThreadHandleLintState *states, AstNode *expr,
                                             bool return_value, bool can_escape);
static void xa_thread_lint_scan_match_expr(XaThreadHandleLintState *states, AstNode *expr,
                                           bool can_escape);
static void xa_thread_lint_scan_select_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                            bool can_escape);
static XaThreadHandleLintState *xa_thread_lint_find_alias_source(XaThreadHandleLintState *states,
                                                                 AstNode *expr);

static void xa_thread_lint_scan_expr_array(XaThreadHandleLintState *states, AstNode **nodes,
                                           int count, bool return_value, bool can_escape) {
    if (!nodes || count <= 0)
        return;
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_expr(states, nodes[i], return_value, can_escape);
}

static XaThreadHandleLintState *xa_thread_lint_find_alias_source(XaThreadHandleLintState *states,
                                                                 AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr)
        return NULL;
    if (expr->type == AST_TERNARY) {
        XaThreadHandleLintState *true_state =
            xa_thread_lint_find_alias_source(states, expr->as.ternary.true_expr);
        XaThreadHandleLintState *false_state =
            xa_thread_lint_find_alias_source(states, expr->as.ternary.false_expr);
        return true_state && true_state == false_state ? true_state : NULL;
    }
    if (expr->type == AST_NULLISH_COALESCE) {
        XaThreadHandleLintState *left_state =
            xa_thread_lint_find_alias_source(states, expr->as.binary.left);
        XaThreadHandleLintState *right_state =
            xa_thread_lint_find_alias_source(states, expr->as.binary.right);
        return left_state && left_state == right_state ? left_state : NULL;
    }
    if (expr->type == AST_MATCH_EXPR) {
        MatchExprNode *match = &expr->as.match_expr;
        XaThreadHandleLintState *matched_state = NULL;
        for (int i = 0; i < match->arm_count; i++) {
            AstNode *arm = match->arms ? match->arms[i] : NULL;
            if (!arm || arm->type != AST_MATCH_ARM)
                return NULL;
            AstNode *body = xa_thread_lint_unwrap_expr(arm->as.match_arm.body);
            if (body && (body->type == AST_BLOCK || body->type == AST_PROGRAM))
                body = xa_lifecycle_lint_single_expr_block_value(body);
            XaThreadHandleLintState *arm_state = xa_thread_lint_find_alias_source(states, body);
            if (!arm_state || (matched_state && matched_state != arm_state))
                return NULL;
            matched_state = arm_state;
        }
        return matched_state;
    }
    if (expr->type == AST_MOVE_EXPR)
        return xa_thread_lint_find_alias_source(states, expr->as.move_expr.expr);
    if (expr->type == AST_AS_EXPR)
        return xa_thread_lint_find_alias_source(states, expr->as.as_expr.expr);
    if (expr->type == AST_UNSAFE_EXPR) {
        AstNode *operand = xa_thread_lint_unwrap_expr(expr->as.unsafe_expr.operand);
        if (!operand)
            return NULL;
        if (operand->type == AST_BLOCK || operand->type == AST_PROGRAM)
            operand = xa_lifecycle_lint_single_expr_block_value(operand);
        return xa_thread_lint_find_alias_source(states, operand);
    }
    XaThreadHandleLintState *state = xa_thread_lint_find_by_expr(states, expr);
    if (!state)
        state = xa_thread_lint_find_returned_call_arg(states, expr);
    return state;
}

static void xa_thread_lint_note_var_alias(XaThreadHandleLintState *states, VarDeclNode *var) {
    if (!states || !var || var->symbol_id == 0 || !var->initializer)
        return;
    if (var->storage_mode != XR_STORAGE_NORMAL)
        return;
    XaThreadHandleLintState *state = xa_thread_lint_find_alias_source(states, var->initializer);
    if (state)
        xa_thread_lint_add_alias_id(state, var->symbol_id);
}

static void xa_thread_lint_note_assignment_alias(XaThreadHandleLintState *states,
                                                 AssignmentNode *assignment) {
    if (!states || !assignment || assignment->symbol_id == 0 || !assignment->value)
        return;
    XaThreadHandleLintState *state = xa_thread_lint_find_alias_source(states, assignment->value);
    xa_thread_lint_remove_alias_id(states, assignment->symbol_id);
    if (state)
        xa_thread_lint_add_alias_id(state, assignment->symbol_id);
}

static AstNode *xa_lifecycle_lint_destructure_source_at(AstNode *initializer, int index) {
    initializer = xa_thread_lint_unwrap_expr(initializer);
    if (!initializer || index < 0)
        return NULL;
    if (initializer->type == AST_TUPLE_LITERAL) {
        TupleLiteralNode *tuple = &initializer->as.tuple_literal;
        if (index < tuple->count)
            return tuple->elements[index];
        return NULL;
    }
    if (initializer->type == AST_ARRAY_LITERAL && !initializer->as.array_literal.is_repeat) {
        ArrayLiteralNode *array = &initializer->as.array_literal;
        if (index < array->count)
            return array->elements[index];
    }
    return NULL;
}

static AstNode *xa_lifecycle_lint_object_source_for_field(AstNode *initializer,
                                                          const char *field_name) {
    initializer = xa_thread_lint_unwrap_expr(initializer);
    if (!initializer || initializer->type != AST_OBJECT_LITERAL || !field_name)
        return NULL;
    ObjectLiteralNode *object = &initializer->as.object_literal;
    for (int i = 0; i < object->count; i++) {
        AstNode *key = xa_thread_lint_unwrap_expr(object->keys ? object->keys[i] : NULL);
        if (!key || key->type != AST_LITERAL_STRING || !key->as.literal.raw_value.string_val)
            continue;
        if (strcmp(key->as.literal.raw_value.string_val, field_name) == 0)
            return object->values ? object->values[i] : NULL;
    }
    return NULL;
}

static void xa_thread_lint_note_destructure_aliases(XaThreadHandleLintState *states,
                                                    XrDestructurePattern *pattern,
                                                    AstNode *initializer, bool invalidate_targets) {
    if (!states || !pattern || !initializer)
        return;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER: {
            if (pattern->as.identifier.symbol_id == 0)
                return;
            XaThreadHandleLintState *state = xa_thread_lint_find_alias_source(states, initializer);
            if (invalidate_targets)
                xa_thread_lint_remove_alias_id(states, pattern->as.identifier.symbol_id);
            if (state)
                xa_thread_lint_add_alias_id(state, pattern->as.identifier.symbol_id);
            return;
        }
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                xa_thread_lint_note_destructure_aliases(
                    states, pattern->as.array.elements[i],
                    xa_lifecycle_lint_destructure_source_at(initializer, i), invalidate_targets);
            }
            return;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                xa_thread_lint_note_destructure_aliases(
                    states, pattern->as.object.patterns[i],
                    xa_lifecycle_lint_object_source_for_field(initializer,
                                                              pattern->as.object.field_names[i]),
                    invalidate_targets);
            }
            return;
        default:
            return;
    }
}

static bool xa_thread_lint_scan_join_or_detach_call(XaThreadHandleLintState *states, AstNode *expr,
                                                    bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || (strcmp(ma->name, "join") != 0 && strcmp(ma->name, "detach") != 0))
        return false;
    XaThreadHandleLintState *state = xa_thread_lint_find_alias_source(states, ma->object);
    if (!state)
        return false;
    if (can_escape)
        state->finalized = true;
    return true;
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_find_fn_summary(XaInferContext *ctx, XaThreadHandleLintFnSummary *summaries,
                               AstNode *callee) {
    callee = xa_thread_lint_unwrap_expr(callee);
    if (!callee || callee->type != AST_VARIABLE || !callee->as.variable.name)
        return NULL;
    uint32_t symbol_id = xa_lifecycle_lint_callee_symbol_id(ctx, callee);
    for (XaThreadHandleLintFnSummary *s = summaries; s; s = s->next) {
        if (!s->name || strcmp(s->name, callee->as.variable.name) != 0)
            continue;
        if (s->symbol_id != 0 && symbol_id != 0) {
            if (s->symbol_id == symbol_id)
                return s;
            continue;
        }
        if (s->symbol_id != 0 || symbol_id != 0)
            continue;
        return s;
    }
    return NULL;
}

static bool xa_thread_lint_scan_helper_finalizer_call(XaThreadHandleLintState *states,
                                                      AstNode *expr, bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr || expr->type != AST_CALL_EXPR || !can_escape)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    XaThreadHandleLintFnSummary *summary =
        xa_thread_lint_find_fn_summary(states->ctx, states->fn_summaries, call->callee);
    if (!summary || !summary->param_finalized)
        return false;
    bool applied = false;
    int n = call->arg_count < summary->param_count ? call->arg_count : summary->param_count;
    for (int i = 0; i < n; i++) {
        if (!summary->param_finalized[i])
            continue;
        XaThreadHandleLintState *state =
            xa_thread_lint_find_alias_source(states, call->arguments[i]);
        if (!state)
            continue;
        state->finalized = true;
        applied = true;
    }
    return applied;
}

static bool xa_thread_lint_expr_returns_new_handle(XaInferContext *ctx,
                                                   XaThreadHandleLintFnSummary *summaries,
                                                   AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    if (xa_expr_is_sys_thread_spawn_call(expr))
        return true;
    switch (expr->type) {
        case AST_TERNARY:
            return xa_thread_lint_expr_returns_new_handle(ctx, summaries,
                                                          expr->as.ternary.true_expr) &&
                   xa_thread_lint_expr_returns_new_handle(ctx, summaries,
                                                          expr->as.ternary.false_expr);
        case AST_NULLISH_COALESCE:
            return xa_thread_lint_expr_returns_new_handle(ctx, summaries, expr->as.binary.left) &&
                   xa_thread_lint_expr_returns_new_handle(ctx, summaries, expr->as.binary.right);
        case AST_MATCH_EXPR: {
            MatchExprNode *match = &expr->as.match_expr;
            if (match->arm_count <= 0)
                return false;
            for (int i = 0; i < match->arm_count; i++) {
                AstNode *arm = match->arms ? match->arms[i] : NULL;
                if (!arm || arm->type != AST_MATCH_ARM)
                    return false;
                AstNode *body = xa_thread_lint_unwrap_expr(arm->as.match_arm.body);
                if (body && (body->type == AST_BLOCK || body->type == AST_PROGRAM))
                    body = xa_lifecycle_lint_single_expr_block_value(body);
                if (!xa_thread_lint_expr_returns_new_handle(ctx, summaries, body))
                    return false;
            }
            return true;
        }
        case AST_CALL_EXPR: {
            XaThreadHandleLintFnSummary *summary =
                xa_thread_lint_find_fn_summary(ctx, summaries, expr->as.call_expr.callee);
            return summary && summary->returns_new_handle;
        }
        case AST_MOVE_EXPR:
            return xa_thread_lint_expr_returns_new_handle(ctx, summaries, expr->as.move_expr.expr);
        case AST_AS_EXPR:
            return xa_thread_lint_expr_returns_new_handle(ctx, summaries, expr->as.as_expr.expr);
        case AST_UNSAFE_EXPR: {
            AstNode *operand = xa_thread_lint_unwrap_expr(expr->as.unsafe_expr.operand);
            if (operand && (operand->type == AST_BLOCK || operand->type == AST_PROGRAM))
                operand = xa_lifecycle_lint_single_expr_block_value(operand);
            return xa_thread_lint_expr_returns_new_handle(ctx, summaries, operand);
        }
        default:
            return false;
    }
}

static XaThreadHandleLintState *
xa_thread_lint_find_returned_call_arg(XaThreadHandleLintState *states, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr || expr->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &expr->as.call_expr;
    XaThreadHandleLintFnSummary *summary =
        xa_thread_lint_find_fn_summary(states->ctx, states->fn_summaries, call->callee);
    if (!summary || summary->return_param_index < 0 ||
        summary->return_param_index >= summary->param_count ||
        summary->return_param_index >= call->arg_count)
        return NULL;
    AstNode *arg = call->arguments[summary->return_param_index];
    return xa_thread_lint_find_alias_source(states, arg);
}

static void xa_thread_lint_scan_expr(XaThreadHandleLintState *states, AstNode *expr,
                                     bool return_value, bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return;

    if (return_value && can_escape) {
        XaThreadHandleLintState *state = xa_thread_lint_find_alias_source(states, expr);
        if (state) {
            state->transferred = true;
            return;
        }
    }

    switch (expr->type) {
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return;

        case AST_CALL_EXPR: {
            if (xa_thread_lint_scan_join_or_detach_call(states, expr, can_escape))
                return;
            xa_thread_lint_scan_helper_finalizer_call(states, expr, can_escape);
            CallExprNode *call = &expr->as.call_expr;
            xa_thread_lint_scan_expr(states, call->callee, false, can_escape);
            xa_thread_lint_scan_expr_array(states, call->arguments, call->arg_count, false,
                                           can_escape);
            return;
        }

        case AST_MEMBER_ACCESS:
            xa_thread_lint_scan_expr(states, expr->as.member_access.object, false, can_escape);
            return;
        case AST_MEMBER_SET:
            xa_thread_lint_scan_expr(states, expr->as.member_set.object, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.member_set.value, false, can_escape);
            return;
        case AST_INDEX_GET:
            xa_thread_lint_scan_expr(states, expr->as.index_get.array, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_get.index, false, can_escape);
            return;
        case AST_INDEX_SET:
            xa_thread_lint_scan_expr(states, expr->as.index_set.array, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_set.index, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_set.value, false, can_escape);
            return;
        case AST_SLICE_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.slice_expr.source, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.slice_expr.start, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.slice_expr.end, false, can_escape);
            return;

        case AST_ASSIGNMENT:
            xa_thread_lint_note_assignment_alias(states, &expr->as.assignment);
            xa_thread_lint_scan_expr(states, expr->as.assignment.value, false, can_escape);
            return;
        case AST_COMPOUND_ASSIGNMENT:
            xa_thread_lint_scan_expr(states, expr->as.compound_assignment.object, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.compound_assignment.value, false, can_escape);
            return;
        case AST_INC:
        case AST_DEC:
            return;

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
        case AST_NULLISH_COALESCE:
            xa_thread_lint_scan_expr(states, expr->as.binary.left, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.binary.right, false, can_escape);
            return;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            xa_thread_lint_scan_expr(states, expr->as.unary.operand, false, can_escape);
            return;
        case AST_GROUPING:
            xa_thread_lint_scan_expr(states, expr->as.grouping, false, can_escape);
            return;

        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat) {
                xa_thread_lint_scan_expr(states, expr->as.array_literal.repeat_value, false,
                                         can_escape);
                xa_thread_lint_scan_expr(states, expr->as.array_literal.repeat_count, false,
                                         can_escape);
            } else {
                xa_thread_lint_scan_expr_array(states, expr->as.array_literal.elements,
                                               expr->as.array_literal.count, false, can_escape);
            }
            return;
        case AST_TUPLE_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.tuple_literal.elements,
                                           expr->as.tuple_literal.count, false, can_escape);
            return;
        case AST_TEMPLATE_STRING:
            xa_thread_lint_scan_expr_array(states, expr->as.template_str.parts,
                                           expr->as.template_str.part_count, false, can_escape);
            return;
        case AST_SPREAD_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.spread_expr.expr, false, can_escape);
            return;
        case AST_OBJECT_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.object_literal.keys,
                                           expr->as.object_literal.count, false, can_escape);
            xa_thread_lint_scan_expr_array(states, expr->as.object_literal.values,
                                           expr->as.object_literal.count, false, can_escape);
            return;
        case AST_MAP_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.map_literal.keys,
                                           expr->as.map_literal.count, false, can_escape);
            xa_thread_lint_scan_expr_array(states, expr->as.map_literal.values,
                                           expr->as.map_literal.count, false, can_escape);
            return;
        case AST_SET_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.set_literal.elements,
                                           expr->as.set_literal.count, false, can_escape);
            return;
        case AST_STRUCT_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.struct_literal.field_values,
                                           expr->as.struct_literal.field_count, false, can_escape);
            return;

        case AST_TERNARY:
            xa_thread_lint_scan_ternary_expr(states, expr, return_value, can_escape);
            return;
        case AST_OPTIONAL_CHAIN:
            xa_thread_lint_scan_expr(states, expr->as.optional_chain.object, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.optional_chain.index, false, can_escape);
            return;
        case AST_RANGE:
            xa_thread_lint_scan_expr(states, expr->as.range.start, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.range.end, false, can_escape);
            return;
        case AST_IS_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.is_expr.expr, false, can_escape);
            return;
        case AST_AS_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.as_expr.expr, false, can_escape);
            return;
        case AST_COMPTIME_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.comptime_expr.expr, false, can_escape);
            return;
        case AST_UNSAFE_EXPR:
            if (expr->as.unsafe_expr.operand &&
                (expr->as.unsafe_expr.operand->type == AST_BLOCK ||
                 expr->as.unsafe_expr.operand->type == AST_PROGRAM)) {
                xa_thread_lint_scan_stmt(states, expr->as.unsafe_expr.operand, can_escape);
            } else {
                xa_thread_lint_scan_expr(states, expr->as.unsafe_expr.operand, false, can_escape);
            }
            return;
        case AST_MOVE_EXPR: {
            xa_thread_lint_scan_expr(states, expr->as.move_expr.expr, false, can_escape);
            return;
        }

        case AST_GO_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.go_expr.expr, false, can_escape);
            return;
        case AST_AWAIT_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.await_expr.expr, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.await_expr.timeout, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.await_expr.into, false, can_escape);
            return;
        case AST_CHANNEL_NEW:
            xa_thread_lint_scan_expr(states, expr->as.channel_new.buffer_size, false, can_escape);
            return;
        case AST_MATCH_EXPR:
            xa_thread_lint_scan_match_expr(states, expr, can_escape);
            return;
        case AST_NEW_EXPR:
            xa_thread_lint_scan_expr_array(states, expr->as.new_expr.arguments,
                                           expr->as.new_expr.arg_count, false, can_escape);
            return;
        case AST_SUPER_CALL:
            xa_thread_lint_scan_expr_array(states, expr->as.super_call.arguments,
                                           expr->as.super_call.arg_count, false, can_escape);
            return;
        case AST_ENUM_INDEX:
            xa_thread_lint_scan_expr(states, expr->as.enum_index.collection, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.enum_index.index_expr, false, can_escape);
            return;
        case AST_YIELD_STMT:
            xa_thread_lint_scan_expr(states, expr->as.yield_stmt.value, false, can_escape);
            return;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
        case AST_METHOD_DECL:
        case AST_FIELD_DECL:
        case AST_ENUM_DECL:
        case AST_ENUM_MEMBER:
        case AST_ENUM_ACCESS:
        case AST_IMPORT_STMT:
        case AST_INTERFACE_DECL:
        case AST_INTERFACE_METHOD:
        case AST_INTERFACE_PROPERTY:
        case AST_TYPE_ALIAS:
        case AST_GLOBAL_ASM:
        case AST_THIS_EXPR:
        case AST_CANCELLED_EXPR:
            return;

        default:
            return;
    }
}

static void xa_thread_lint_scan_ternary_expr(XaThreadHandleLintState *states, AstNode *expr,
                                             bool return_value, bool can_escape) {
    if (!expr || expr->type != AST_TERNARY)
        return;
    TernaryNode *ternary = &expr->as.ternary;
    xa_thread_lint_scan_expr(states, ternary->condition, false, can_escape);

    if (!can_escape) {
        xa_thread_lint_scan_expr(states, ternary->true_expr, return_value, false);
        xa_thread_lint_scan_expr(states, ternary->false_expr, return_value, false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *true_after = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *false_after = xr_calloc(1, snapshot_size);
    if (!before || !true_after || !false_after) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(true_after, state_count);
        xa_thread_lint_free_snapshot_array(false_after, state_count);
        xa_thread_lint_scan_expr(states, ternary->true_expr, return_value, false);
        xa_thread_lint_scan_expr(states, ternary->false_expr, return_value, false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    xa_thread_lint_scan_expr(states, ternary->true_expr, return_value, true);
    xa_thread_lint_snapshot_states(states, true_after);

    xa_thread_lint_restore_states(states, before);
    xa_thread_lint_scan_expr(states, ternary->false_expr, return_value, true);
    xa_thread_lint_snapshot_states(states, false_after);

    xa_thread_lint_restore_states(states, before);
    XaThreadHandleLintSnapshot *alias_paths[] = {true_after, false_after};
    xa_thread_lint_merge_alias_snapshots(states, before, alias_paths, 2, state_count);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        bool true_closed = xa_thread_lint_snapshot_closed(&true_after[i]);
        bool false_closed = xa_thread_lint_snapshot_closed(&false_after[i]);
        if (!before_closed && true_closed && false_closed)
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(true_after, state_count);
    xa_thread_lint_free_snapshot_array(false_after, state_count);
}

static void xa_thread_lint_scan_block(XaThreadHandleLintState *states, AstNode *block,
                                      bool can_escape) {
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block, &statements, &count))
        return;
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_stmt(states, statements[i], can_escape);
}

static void xa_thread_lint_scan_if_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                        bool can_escape) {
    xa_thread_lint_scan_expr(states, stmt->as.if_stmt.condition, false, can_escape);
    if (!can_escape) {
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *then_after = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *else_after = xr_calloc(1, snapshot_size);
    if (!before || !then_after || !else_after) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(then_after, state_count);
        xa_thread_lint_free_snapshot_array(else_after, state_count);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, true);
    xa_thread_lint_snapshot_states(states, then_after);

    xa_thread_lint_restore_states(states, before);
    if (stmt->as.if_stmt.else_branch)
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, true);
    xa_thread_lint_snapshot_states(states, else_after);

    xa_thread_lint_restore_states(states, before);
    XaThreadHandleLintSnapshot *alias_paths[] = {then_after, else_after};
    xa_thread_lint_merge_alias_snapshots(states, before, alias_paths, 2, state_count);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        bool then_closed = xa_thread_lint_snapshot_closed(&then_after[i]);
        bool else_closed = xa_thread_lint_snapshot_closed(&else_after[i]);
        if (!before_closed && then_closed && else_closed)
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(then_after, state_count);
    xa_thread_lint_free_snapshot_array(else_after, state_count);
}

static void xa_thread_lint_scan_try_catch_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                               bool can_escape) {
    if (!stmt || stmt->type != AST_TRY_CATCH)
        return;
    TryCatchNode *tc = &stmt->as.try_catch;

    if (!can_escape) {
        xa_thread_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_thread_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *try_after = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *catch_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !try_after || !catch_after || !all_paths_closed) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(try_after, state_count);
        xa_thread_lint_free_snapshot_array(catch_after, state_count);
        xr_free(all_paths_closed);
        xa_thread_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_thread_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    xa_thread_lint_scan_stmt(states, tc->try_body, true);
    xa_thread_lint_snapshot_states(states, try_after);

    int alias_path_count = 1 + tc->catch_count;
    XaThreadHandleLintSnapshot **alias_paths =
        alias_path_count > 0 ? xr_calloc((size_t) alias_path_count, sizeof(*alias_paths)) : NULL;
    bool alias_paths_complete = alias_paths != NULL;
    if (alias_paths)
        alias_paths[0] = try_after;

    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = xa_thread_lint_snapshot_closed(&try_after[i]);

    for (int ci = 0; ci < tc->catch_count; ci++) {
        XrCatchClause *cc = tc->catch_clauses[ci];
        if (!cc) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_thread_lint_restore_states(states, before);
        XaThreadHandleLintSnapshot *path_after = catch_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ci + 1] = path_after;
            else {
                alias_paths_complete = false;
                path_after = catch_after;
            }
        }
        if (path_after == catch_after)
            xa_thread_lint_clear_snapshot_array(catch_after, state_count);
        xa_thread_lint_scan_stmt(states, cc->body, true);
        xa_thread_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_thread_lint_snapshot_closed(&path_after[i]);
    }

    xa_thread_lint_restore_states(states, before);
    if (alias_paths_complete)
        xa_thread_lint_merge_alias_snapshots(states, before, alias_paths, alias_path_count,
                                             state_count);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_paths(alias_paths, alias_path_count, state_count, try_after,
                                       catch_after);
    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(try_after, state_count);
    xa_thread_lint_free_snapshot_array(catch_after, state_count);
    xr_free(all_paths_closed);
}

static void xa_thread_lint_scan_match_expr(XaThreadHandleLintState *states, AstNode *expr,
                                           bool can_escape) {
    if (!expr || expr->type != AST_MATCH_EXPR)
        return;
    MatchExprNode *match = &expr->as.match_expr;
    xa_thread_lint_scan_expr(states, match->expr, false, can_escape);

    if (!can_escape) {
        for (int i = 0; i < match->arm_count; i++)
            xa_thread_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *arm_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !arm_after || !all_paths_closed) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(arm_after, state_count);
        xr_free(all_paths_closed);
        for (int i = 0; i < match->arm_count; i++)
            xa_thread_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = match->arm_count > 0;

    XaThreadHandleLintSnapshot **alias_paths =
        match->arm_count > 0 ? xr_calloc((size_t) match->arm_count, sizeof(*alias_paths)) : NULL;
    bool alias_paths_complete = match->arm_count > 0 && alias_paths != NULL;

    for (int ai = 0; ai < match->arm_count; ai++) {
        if (!match->arms[ai]) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_thread_lint_restore_states(states, before);
        XaThreadHandleLintSnapshot *path_after = arm_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ai] = path_after;
            else {
                alias_paths_complete = false;
                path_after = arm_after;
            }
        }
        if (path_after == arm_after)
            xa_thread_lint_clear_snapshot_array(arm_after, state_count);
        xa_thread_lint_scan_stmt(states, match->arms[ai], true);
        xa_thread_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_thread_lint_snapshot_closed(&path_after[i]);
    }

    xa_thread_lint_restore_states(states, before);
    if (alias_paths_complete)
        xa_thread_lint_merge_alias_snapshots(states, before, alias_paths, match->arm_count,
                                             state_count);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_paths(alias_paths, match->arm_count, state_count, arm_after, NULL);
    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(arm_after, state_count);
    xr_free(all_paths_closed);
}

static void xa_thread_lint_scan_select_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                            bool can_escape) {
    if (!stmt || stmt->type != AST_SELECT_STMT)
        return;
    SelectStmtNode *select = &stmt->as.select_stmt;

    if (!can_escape) {
        for (int i = 0; i < select->case_count; i++)
            xa_thread_lint_scan_stmt(states, select->cases[i], false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *case_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !case_after || !all_paths_closed) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(case_after, state_count);
        xr_free(all_paths_closed);
        for (int i = 0; i < select->case_count; i++)
            xa_thread_lint_scan_stmt(states, select->cases[i], false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = select->case_count > 0;

    XaThreadHandleLintSnapshot **alias_paths =
        select->case_count > 0 ? xr_calloc((size_t) select->case_count, sizeof(*alias_paths))
                               : NULL;
    bool alias_paths_complete = select->case_count > 0 && alias_paths != NULL;

    for (int ci = 0; ci < select->case_count; ci++) {
        if (!select->cases[ci]) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_thread_lint_restore_states(states, before);
        XaThreadHandleLintSnapshot *path_after = case_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ci] = path_after;
            else {
                alias_paths_complete = false;
                path_after = case_after;
            }
        }
        if (path_after == case_after)
            xa_thread_lint_clear_snapshot_array(case_after, state_count);
        xa_thread_lint_scan_stmt(states, select->cases[ci], true);
        xa_thread_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_thread_lint_snapshot_closed(&path_after[i]);
    }

    xa_thread_lint_restore_states(states, before);
    if (alias_paths_complete)
        xa_thread_lint_merge_alias_snapshots(states, before, alias_paths, select->case_count,
                                             state_count);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_paths(alias_paths, select->case_count, state_count, case_after,
                                       NULL);
    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(case_after, state_count);
    xr_free(all_paths_closed);
}

static bool xa_thread_lint_mark_linear_finalizer_break_loop(XaThreadHandleLintState *states,
                                                            AstNode *stmt) {
    if (!states || !stmt || !xa_lifecycle_lint_loop_always_enters(states->ctx, stmt))
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(xa_lifecycle_lint_loop_body(stmt), &statements, &count) ||
        count <= 0)
        return false;

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return false;
    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(after, state_count);
        return false;
    }

    const char *loop_label = xa_lifecycle_lint_loop_label(stmt);
    xa_thread_lint_snapshot_states(states, before);
    for (int i = 0; i < count; i++) {
        AstNode *child = statements[i];
        if (xa_lifecycle_lint_branch_is_loop_break(child, loop_label)) {
            xa_thread_lint_snapshot_states(states, after);
            bool closed = false;
            for (int si = 0; si < state_count; si++) {
                if (!xa_thread_lint_snapshot_closed(&before[si]) &&
                    xa_thread_lint_snapshot_closed(&after[si])) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                xa_thread_lint_restore_states(states, before);
            xa_thread_lint_free_snapshot_array(before, state_count);
            xa_thread_lint_free_snapshot_array(after, state_count);
            return closed;
        }
        if (xa_lifecycle_lint_node_skips_loop_tail(child, loop_label)) {
            xa_thread_lint_restore_states(states, before);
            xa_thread_lint_free_snapshot_array(before, state_count);
            xa_thread_lint_free_snapshot_array(after, state_count);
            return false;
        }
        xa_thread_lint_scan_stmt(states, child, true);
    }

    xa_thread_lint_restore_states(states, before);
    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(after, state_count);
    return false;
}

static bool xa_thread_lint_mark_nonempty_for_in_finalizer_loop(XaThreadHandleLintState *states,
                                                               AstNode *stmt) {
    if (!states || !xa_lifecycle_lint_for_in_always_enters(states->ctx, stmt))
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(stmt->as.for_in_stmt.body, &statements, &count) || count <= 0)
        return false;

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return false;
    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(after, state_count);
        return false;
    }

    const char *loop_label = stmt->as.for_in_stmt.label;
    xa_thread_lint_snapshot_states(states, before);
    for (int i = 0; i < count; i++) {
        AstNode *child = statements[i];
        if (xa_lifecycle_lint_node_skips_loop_tail(child, loop_label)) {
            xa_thread_lint_snapshot_states(states, after);
            bool closed = false;
            for (int si = 0; si < state_count; si++) {
                if (!xa_thread_lint_snapshot_closed(&before[si]) &&
                    xa_thread_lint_snapshot_closed(&after[si])) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                xa_thread_lint_restore_states(states, before);
            xa_thread_lint_free_snapshot_array(before, state_count);
            xa_thread_lint_free_snapshot_array(after, state_count);
            return closed;
        }
        xa_thread_lint_scan_stmt(states, child, true);
    }

    xa_thread_lint_snapshot_states(states, after);
    bool closed = false;
    for (int si = 0; si < state_count; si++) {
        if (!xa_thread_lint_snapshot_closed(&before[si]) &&
            xa_thread_lint_snapshot_closed(&after[si])) {
            closed = true;
            break;
        }
    }
    if (!closed)
        xa_thread_lint_restore_states(states, before);
    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(after, state_count);
    return closed;
}

static void xa_thread_lint_scan_defer_expr(XaThreadHandleLintState *states, AstNode *expr,
                                           bool can_escape) {
    if (!states || !expr || !can_escape)
        return;

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;
    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_thread_lint_free_snapshot_array(before, state_count);
        xa_thread_lint_free_snapshot_array(after, state_count);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    AstNode *deferred = xa_thread_lint_unwrap_expr(expr);
    if (deferred && deferred->type == AST_FUNCTION_EXPR)
        xa_thread_lint_scan_stmt(states, deferred->as.function_expr.body, true);
    else
        xa_thread_lint_scan_expr(states, deferred, false, true);
    xa_thread_lint_snapshot_states(states, after);

    xa_thread_lint_restore_states(states, before);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        if (!xa_thread_lint_snapshot_closed(&before[i]) && after[i].finalized)
            s->finalized = true;
    }

    xa_thread_lint_free_snapshot_array(before, state_count);
    xa_thread_lint_free_snapshot_array(after, state_count);
}

static void xa_thread_lint_scan_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                     bool can_escape) {
    if (!stmt)
        return;
    switch (stmt->type) {
        case AST_EXPR_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.expr_stmt, false, can_escape);
            return;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            xa_thread_lint_note_var_alias(states, &stmt->as.var_decl);
            xa_thread_lint_scan_expr(states, stmt->as.var_decl.initializer, false, can_escape);
            return;
        case AST_DESTRUCTURE_DECL:
            xa_thread_lint_note_destructure_aliases(states, stmt->as.destructure_decl.pattern,
                                                    stmt->as.destructure_decl.initializer, false);
            xa_thread_lint_scan_expr(states, stmt->as.destructure_decl.initializer, false,
                                     can_escape);
            return;
        case AST_DESTRUCTURE_ASSIGN:
            xa_thread_lint_note_destructure_aliases(states, stmt->as.destructure_assign.pattern,
                                                    stmt->as.destructure_assign.value, true);
            xa_thread_lint_scan_expr(states, stmt->as.destructure_assign.value, false, can_escape);
            return;
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
            xa_thread_lint_scan_expr(states, stmt, false, can_escape);
            return;
        case AST_PRINT_STMT:
            xa_thread_lint_scan_expr_array(states, stmt->as.print_stmt.exprs,
                                           stmt->as.print_stmt.expr_count, false, can_escape);
            return;
        case AST_RETURN_STMT:
            xa_thread_lint_scan_expr_array(states, stmt->as.return_stmt.values,
                                           stmt->as.return_stmt.value_count, true, can_escape);
            return;
        case AST_IF_STMT:
            xa_thread_lint_scan_if_stmt(states, stmt, can_escape);
            return;
        case AST_WHILE_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.while_stmt.condition, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.while_stmt.body, false);
            if (can_escape)
                xa_thread_lint_mark_linear_finalizer_break_loop(states, stmt);
            return;
        case AST_FOR_STMT:
            xa_thread_lint_scan_stmt(states, stmt->as.for_stmt.initializer, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.for_stmt.condition, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.for_stmt.increment, false, false);
            xa_thread_lint_scan_stmt(states, stmt->as.for_stmt.body, false);
            if (can_escape)
                xa_thread_lint_mark_linear_finalizer_break_loop(states, stmt);
            return;
        case AST_FOR_IN_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.for_in_stmt.collection, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.for_in_stmt.body, false);
            if (can_escape)
                xa_thread_lint_mark_nonempty_for_in_finalizer_loop(states, stmt);
            return;
        case AST_BLOCK:
        case AST_PROGRAM:
            xa_thread_lint_scan_block(states, stmt, can_escape);
            return;
        case AST_TRY_CATCH:
            xa_thread_lint_scan_try_catch_stmt(states, stmt, can_escape);
            return;
        case AST_MATCH_EXPR:
            xa_thread_lint_scan_match_expr(states, stmt, can_escape);
            return;
        case AST_MATCH_ARM:
            xa_thread_lint_scan_expr(states, stmt->as.match_arm.pattern, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.match_arm.guard, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.match_arm.body, can_escape);
            return;
        case AST_SELECT_STMT:
            xa_thread_lint_scan_select_stmt(states, stmt, can_escape);
            return;
        case AST_SELECT_CASE:
            xa_thread_lint_scan_expr(states, stmt->as.select_case.channel, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.select_case.value, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.select_case.body, can_escape);
            return;
        case AST_THROW_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.throw_stmt.expression, false, can_escape);
            return;
        case AST_DEFER_STMT:
            xa_thread_lint_scan_defer_expr(states, stmt->as.defer_stmt.expr, can_escape);
            return;
        case AST_SCOPE_BLOCK:
            xa_thread_lint_scan_stmt(states, stmt->as.scope_block.body, can_escape);
            return;
        case AST_EXPORT_STMT:
            xa_thread_lint_scan_stmt(states, stmt->as.export_stmt.declaration, can_escape);
            return;
        default:
            xa_thread_lint_scan_expr(states, stmt, false, can_escape);
            return;
    }
}

static XaThreadHandleLintState **xa_thread_lint_append_spawn_state(XaInferContext *ctx,
                                                                   AstNode *decl_stmt,
                                                                   XaSymbol *sym,
                                                                   XaThreadHandleLintState **tail);

static XaThreadHandleLintState **xa_thread_lint_collect_destructure_spawn_states(
    XaInferContext *ctx, AstNode *decl_stmt, XrDestructurePattern *pattern, AstNode *initializer,
    XaThreadHandleLintFnSummary *fn_summaries, XaThreadHandleLintState **tail);

static XaThreadHandleLintState *
xa_thread_lint_collect_spawn_states(XaInferContext *ctx, AstNode **statements, int count,
                                    XaThreadHandleLintFnSummary *fn_summaries) {
    if (!ctx || !statements || count <= 0)
        return NULL;
    XaThreadHandleLintState *states = NULL;
    XaThreadHandleLintState **tail = &states;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_VAR_DECL || stmt->type == AST_CONST_DECL) {
            VarDeclNode *var = &stmt->as.var_decl;
            if (!var->name || !var->initializer ||
                (!xa_expr_is_sys_thread_spawn_call(var->initializer) &&
                 !xa_thread_lint_expr_returns_new_handle(ctx, fn_summaries, var->initializer)))
                continue;
            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
            tail = xa_thread_lint_append_spawn_state(ctx, stmt, sym, tail);
            continue;
        }
        if (stmt->type == AST_DESTRUCTURE_DECL) {
            tail = xa_thread_lint_collect_destructure_spawn_states(
                ctx, stmt, stmt->as.destructure_decl.pattern, stmt->as.destructure_decl.initializer,
                fn_summaries, tail);
            continue;
        }
    }
    return states;
}

static XaThreadHandleLintState **xa_thread_lint_append_spawn_state(XaInferContext *ctx,
                                                                   AstNode *decl_stmt,
                                                                   XaSymbol *sym,
                                                                   XaThreadHandleLintState **tail) {
    if (!ctx || !tail || !sym)
        return tail;
    if (!xa_symbol_is_local_thread_handle(sym, ctx->analyzer->current_scope))
        return tail;
    XaThreadHandleLintState *state = xr_calloc(1, sizeof(XaThreadHandleLintState));
    if (!state)
        return tail;
    state->ctx = ctx;
    state->root = sym;
    state->decl_stmt = decl_stmt;
    xa_thread_lint_add_alias(state, sym);
    *tail = state;
    return &state->next;
}

static XaThreadHandleLintState **xa_thread_lint_collect_destructure_spawn_states(
    XaInferContext *ctx, AstNode *decl_stmt, XrDestructurePattern *pattern, AstNode *initializer,
    XaThreadHandleLintFnSummary *fn_summaries, XaThreadHandleLintState **tail) {
    if (!ctx || !ctx->analyzer || !pattern || !initializer || !tail)
        return tail;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER: {
            if (!xa_thread_lint_expr_returns_new_handle(ctx, fn_summaries, initializer))
                return tail;
            XaSymbol *sym = NULL;
            if (pattern->as.identifier.symbol_id != 0)
                sym = xa_scope_lookup_by_id(ctx->analyzer->current_scope,
                                            pattern->as.identifier.symbol_id);
            if (!sym && pattern->as.identifier.name)
                sym = xa_scope_lookup(ctx->analyzer->current_scope, pattern->as.identifier.name);
            return xa_thread_lint_append_spawn_state(ctx, decl_stmt, sym, tail);
        }
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                tail = xa_thread_lint_collect_destructure_spawn_states(
                    ctx, decl_stmt, pattern->as.array.elements[i],
                    xa_lifecycle_lint_destructure_source_at(initializer, i), fn_summaries, tail);
            }
            return tail;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                tail = xa_thread_lint_collect_destructure_spawn_states(
                    ctx, decl_stmt, pattern->as.object.patterns[i],
                    xa_lifecycle_lint_object_source_for_field(initializer,
                                                              pattern->as.object.field_names[i]),
                    fn_summaries, tail);
            }
            return tail;
        default:
            return tail;
    }
}

static void xa_thread_lint_collect_sequence_aliases(XaInferContext *ctx,
                                                    XaThreadHandleLintState *states,
                                                    AstNode **statements, int count) {
    if (!ctx || !states || !statements || count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt || (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
            continue;
        VarDeclNode *var = &stmt->as.var_decl;
        if (!var->name || !var->initializer)
            continue;
        XaThreadHandleLintState *state = xa_thread_lint_find_by_expr(states, var->initializer);
        if (!state)
            continue;
        XaSymbol *alias = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (xa_symbol_is_local_thread_handle(alias, ctx->analyzer->current_scope))
            xa_thread_lint_add_alias(state, alias);
    }
}

static FunctionDeclNode *xa_lifecycle_lint_function_node(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL)
        return &node->as.function_decl;
    if (node->type == AST_FUNCTION_EXPR)
        return &node->as.function_expr;
    return NULL;
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_summarize_function_node(XaInferContext *ctx, AstNode *fn_node,
                                       const char *summary_name, uint32_t summary_symbol_id,
                                       XaThreadHandleLintFnSummary *visible_summaries) {
    FunctionDeclNode *fn = xa_lifecycle_lint_function_node(fn_node);
    if (!ctx || !fn_node || !fn || !summary_name || !fn->body ||
        xa_lifecycle_lint_body_has_non_tail_exit(fn->body))
        return NULL;

    XaThreadHandleLintFnSummary *summary = xr_calloc(1, sizeof(XaThreadHandleLintFnSummary));
    if (!summary)
        return NULL;
    summary->name = summary_name;
    summary->symbol_id = summary_symbol_id;
    summary->return_param_index = -1;
    summary->param_count = fn->param_count;
    summary->param_finalized =
        fn->param_count > 0 ? xr_calloc((size_t) fn->param_count, sizeof(bool)) : NULL;
    XaThreadHandleLintState **by_index =
        fn->param_count > 0 ? xr_calloc((size_t) fn->param_count, sizeof(XaThreadHandleLintState *))
                            : NULL;
    if ((fn->param_count > 0 && !summary->param_finalized) || (fn->param_count > 0 && !by_index)) {
        xr_free(by_index);
        xa_thread_lint_free_fn_summaries(summary);
        return NULL;
    }

    XaThreadHandleLintState *states = NULL;
    XaThreadHandleLintState **tail = &states;
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        if (!param || !xa_lifecycle_lint_function_param_is_named_class(ctx, fn, i, "Thread"))
            continue;
        XaThreadHandleLintState *state = xr_calloc(1, sizeof(XaThreadHandleLintState));
        if (!state)
            continue;
        state->ctx = ctx;
        if (param->symbol_id != 0)
            xa_thread_lint_add_alias_id(state, param->symbol_id);
        xa_thread_lint_add_alias_name(state, param->name);
        XaSymbol *param_sym = xa_lifecycle_lint_function_param_symbol(ctx, fn_node, param);
        if (param_sym)
            xa_thread_lint_add_alias(state, param_sym);
        by_index[i] = state;
        *tail = state;
        tail = &state->next;
    }

    AstNode *return_expr =
        xa_lifecycle_lint_returned_handle_expr(xa_lifecycle_lint_tail_return_expr(fn->body));
    summary->returns_new_handle =
        xa_thread_lint_expr_returns_new_handle(ctx, visible_summaries, return_expr);

    XaThreadHandleLintState *returned_state = NULL;
    if (states) {
        for (XaThreadHandleLintState *s = states; s; s = s->next)
            s->fn_summaries = visible_summaries;
        xa_thread_lint_scan_stmt(states, fn->body, true);
        returned_state = xa_thread_lint_find_alias_source(states, return_expr);
    }
    bool any_finalized = false;
    bool returns_param = false;
    for (int i = 0; i < fn->param_count; i++) {
        XaThreadHandleLintState *state = by_index[i];
        if (!state)
            continue;
        if (state == returned_state) {
            summary->return_param_index = i;
            returns_param = true;
        }
        summary->param_finalized[i] = state->finalized && !state->transferred;
        any_finalized = any_finalized || summary->param_finalized[i];
    }

    xa_thread_lint_free_states(states);
    xr_free(by_index);
    if (!any_finalized && !returns_param && !summary->returns_new_handle) {
        xa_thread_lint_free_fn_summaries(summary);
        return NULL;
    }
    return summary;
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_summarize_function(XaInferContext *ctx, AstNode *stmt,
                                  XaThreadHandleLintFnSummary *visible_summaries) {
    if (!stmt || stmt->type != AST_FUNCTION_DECL)
        return NULL;
    FunctionDeclNode *fn = &stmt->as.function_decl;
    return xa_thread_lint_summarize_function_node(ctx, stmt, fn->name, fn->symbol_id,
                                                  visible_summaries);
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_clone_fn_summary_as(XaThreadHandleLintFnSummary *source, const char *name,
                                   uint32_t symbol_id) {
    if (!source || !name)
        return NULL;
    XaThreadHandleLintFnSummary *summary = xr_calloc(1, sizeof(XaThreadHandleLintFnSummary));
    if (!summary)
        return NULL;
    summary->name = name;
    summary->symbol_id = symbol_id;
    summary->return_param_index = source->return_param_index;
    summary->returns_new_handle = source->returns_new_handle;
    summary->param_count = source->param_count;
    summary->param_finalized =
        source->param_count > 0 ? xr_calloc((size_t) source->param_count, sizeof(bool)) : NULL;
    if (source->param_count > 0 && !summary->param_finalized) {
        xa_thread_lint_free_fn_summaries(summary);
        return NULL;
    }
    if (source->param_count > 0 && source->param_finalized)
        memcpy(summary->param_finalized, source->param_finalized,
               sizeof(bool) * (size_t) source->param_count);
    return summary;
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_summarize_const_function_value(XaInferContext *ctx, AstNode *stmt,
                                              XaThreadHandleLintFnSummary *visible_summaries) {
    if (!stmt || stmt->type != AST_CONST_DECL)
        return NULL;
    VarDeclNode *var = &stmt->as.var_decl;
    AstNode *initializer = xa_thread_lint_unwrap_expr(var->initializer);
    if (!var->name || var->storage_mode != XR_STORAGE_NORMAL || !initializer)
        return NULL;
    if (initializer->type == AST_FUNCTION_EXPR)
        return xa_thread_lint_summarize_function_node(ctx, initializer, var->name, var->symbol_id,
                                                      visible_summaries);
    if (initializer->type != AST_VARIABLE)
        return NULL;
    XaThreadHandleLintFnSummary *source =
        xa_thread_lint_find_fn_summary(ctx, visible_summaries, initializer);
    return xa_thread_lint_clone_fn_summary_as(source, var->name, var->symbol_id);
}

static bool xa_thread_lint_fn_summary_exists(XaThreadHandleLintFnSummary *summaries,
                                             uint32_t symbol_id, const char *name) {
    for (XaThreadHandleLintFnSummary *s = summaries; s; s = s->next) {
        if (symbol_id != 0 && s->symbol_id == symbol_id)
            return true;
        if (symbol_id == 0 && s->symbol_id == 0 && name && s->name && strcmp(s->name, name) == 0)
            return true;
    }
    return false;
}

static bool xa_thread_lint_collect_statement_fn_summaries(XaInferContext *ctx, AstNode **statements,
                                                          int count,
                                                          XaThreadHandleLintFnSummary **head,
                                                          XaThreadHandleLintFnSummary ***tail) {
    if (!ctx || !statements || count <= 0 || !head || !tail || !*tail)
        return false;
    bool added = false;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        XaThreadHandleLintFnSummary *summary = NULL;
        if (stmt->type == AST_FUNCTION_DECL) {
            FunctionDeclNode *fn = &stmt->as.function_decl;
            if (!xa_thread_lint_fn_summary_exists(*head, fn->symbol_id, fn->name))
                summary = xa_thread_lint_summarize_function(ctx, stmt, *head);
        } else if (stmt->type == AST_CONST_DECL) {
            VarDeclNode *var = &stmt->as.var_decl;
            if (!xa_thread_lint_fn_summary_exists(*head, var->symbol_id, var->name))
                summary = xa_thread_lint_summarize_const_function_value(ctx, stmt, *head);
        }
        if (!summary)
            continue;
        **tail = summary;
        *tail = &summary->next;
        added = true;
    }
    return added;
}

static bool xa_thread_lint_collect_scope_fn_summaries(XaInferContext *ctx, XaScope *scope,
                                                      XaThreadHandleLintFnSummary **head,
                                                      XaThreadHandleLintFnSummary ***tail) {
    if (!ctx || !scope || !head || !tail || !*tail)
        return false;
    bool added = false;
    AstNode *node = scope->kind == XA_SCOPE_FUNCTION ? (AstNode *) scope->ast_node : NULL;
    if (node && node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        if (!xa_thread_lint_fn_summary_exists(*head, fn->symbol_id, fn->name)) {
            XaThreadHandleLintFnSummary *summary =
                xa_thread_lint_summarize_function(ctx, node, *head);
            if (summary) {
                **tail = summary;
                *tail = &summary->next;
                added = true;
            }
        }
    }
    int symbol_count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &symbol_count);
    for (int i = 0; i < symbol_count; i++) {
        XaSymbol *sym = symbols ? symbols[i] : NULL;
        if (!sym || sym->kind != XA_SYM_VARIABLE || !sym->is_const || sym->is_shared ||
            sym->is_imported || !sym->name)
            continue;
        if (xa_thread_lint_fn_summary_exists(*head, sym->id, sym->name))
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
        AstNode *initializer = links ? xa_thread_lint_unwrap_expr(links->const_initializer) : NULL;
        if (!initializer ||
            (initializer->type != AST_FUNCTION_EXPR && initializer->type != AST_VARIABLE))
            continue;
        XaThreadHandleLintFnSummary *summary = NULL;
        if (initializer->type == AST_FUNCTION_EXPR) {
            summary =
                xa_thread_lint_summarize_function_node(ctx, initializer, sym->name, sym->id, *head);
        } else {
            XaThreadHandleLintFnSummary *source =
                xa_thread_lint_find_fn_summary(ctx, *head, initializer);
            summary = xa_thread_lint_clone_fn_summary_as(source, sym->name, sym->id);
        }
        if (summary) {
            **tail = summary;
            *tail = &summary->next;
            added = true;
        }
    }
    xr_free(symbols);
    for (int i = 0; i < scope->child_count; i++)
        added =
            xa_thread_lint_collect_scope_fn_summaries(ctx, scope->children[i], head, tail) || added;
    return added;
}

static XaThreadHandleLintFnSummary *
xa_thread_lint_collect_visible_fn_summaries(XaInferContext *ctx, AstNode **statements, int count) {
    XaThreadHandleLintFnSummary *summaries = NULL;
    XaThreadHandleLintFnSummary **tail = &summaries;
    XaScope *root = xa_lifecycle_lint_current_root_scope(ctx);
    bool added = false;
    do {
        added = xa_thread_lint_collect_statement_fn_summaries(ctx, statements, count, &summaries,
                                                              &tail);
        if (root)
            added =
                xa_thread_lint_collect_scope_fn_summaries(ctx, root, &summaries, &tail) || added;
        if (ctx && ctx->analyzer && ctx->analyzer->global_scope &&
            ctx->analyzer->global_scope != root) {
            added = xa_thread_lint_collect_scope_fn_summaries(ctx, ctx->analyzer->global_scope,
                                                              &summaries, &tail) ||
                    added;
        }
    } while (added);
    return summaries;
}

static void xa_thread_lint_attach_fn_summaries(XaThreadHandleLintState *states,
                                               XaThreadHandleLintFnSummary *summaries) {
    for (XaThreadHandleLintState *s = states; s; s = s->next)
        s->fn_summaries = summaries;
}

static void xa_warn_unused_sys_thread_spawn_decl(XaInferContext *ctx, AstNode *stmt,
                                                 XaThreadHandleLintFnSummary *fn_summaries) {
    if (!ctx || !ctx->analyzer || !stmt ||
        (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
        return;

    VarDeclNode *var = &stmt->as.var_decl;
    if (!var->name || !var->initializer ||
        (!xa_expr_is_sys_thread_spawn_call(var->initializer) &&
         !xa_thread_lint_expr_returns_new_handle(ctx, fn_summaries, var->initializer)))
        return;

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!xa_symbol_is_local_thread_handle(sym, ctx->analyzer->current_scope))
        return;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || links->ref_count != 0)
        return;

    char msg[192];
    snprintf(msg, sizeof(msg),
             "Thread handle '%s' from sys.Thread.spawn is never used; call join() or detach() "
             "explicitly",
             var->name);
    XrLocation loc = {.file = ctx->file_path, .line = stmt->line, .column = stmt->column};
    if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_warn_discarded_sys_thread_factory_expr(XaInferContext *ctx, AstNode *stmt,
                                                      XaThreadHandleLintFnSummary *fn_summaries) {
    if (!ctx || !ctx->analyzer || !stmt || stmt->type != AST_EXPR_STMT)
        return;
    AstNode *expr = stmt->as.expr_stmt;
    if (!xa_thread_lint_expr_returns_new_handle(ctx, fn_summaries, expr))
        return;
    const char *msg =
        "sys.Thread.spawn returns a Thread handle; call join() or detach() explicitly";
    XrLocation loc = {.file = ctx->file_path,
                      .line = expr ? expr->line : stmt->line,
                      .column = expr ? expr->column : stmt->column};
    if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_warn_sys_thread_lifecycle_in_sequence(XaInferContext *ctx, AstNode **statements,
                                                     int count, int error_count_before) {
    if (!ctx || !statements || count <= 0)
        return;
    if (xa_analyzer_error_diagnostic_count(ctx->analyzer) != error_count_before)
        return;

    XaThreadHandleLintFnSummary *fn_summaries =
        xa_thread_lint_collect_visible_fn_summaries(ctx, statements, count);

    for (int i = 0; i < count; i++) {
        xa_warn_unused_sys_thread_spawn_decl(ctx, statements[i], fn_summaries);
        xa_warn_discarded_sys_thread_factory_expr(ctx, statements[i], fn_summaries);
    }

    XaThreadHandleLintState *states =
        xa_thread_lint_collect_spawn_states(ctx, statements, count, fn_summaries);
    if (!states) {
        xa_thread_lint_free_fn_summaries(fn_summaries);
        return;
    }
    xa_thread_lint_collect_sequence_aliases(ctx, states, statements, count);
    xa_thread_lint_attach_fn_summaries(states, fn_summaries);
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_stmt(states, statements[i], true);
    for (XaThreadHandleLintState *state = states; state; state = state->next) {
        if (!state->root || !state->decl_stmt || state->finalized || state->transferred)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, state->root);
        if ((!links || links->ref_count == 0) && state->decl_stmt->type != AST_DESTRUCTURE_DECL)
            continue;
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "Thread handle '%s' from sys.Thread.spawn is not joined or detached before "
                 "leaving scope",
                 state->root->name ? state->root->name : "?");
        XrLocation loc = {.file = ctx->file_path,
                          .line = state->decl_stmt->line,
                          .column = state->decl_stmt->column};
        if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg,
                                       &loc);
    }
    xa_thread_lint_free_states(states);
    xa_thread_lint_free_fn_summaries(fn_summaries);
}

typedef enum XaOsResourceKind {
    XA_OS_RESOURCE_PROCESS,
    XA_OS_RESOURCE_PIPE,
} XaOsResourceKind;

typedef struct XaOsResourceAlias {
    XaSymbol *sym;
    uint32_t symbol_id;
    const char *name;
    struct XaOsResourceAlias *next;
} XaOsResourceAlias;

typedef struct XaOsResourceLintFnSummary XaOsResourceLintFnSummary;

typedef struct XaOsResourceLintState {
    XaInferContext *ctx;
    XaOsResourceKind kind;
    XaSymbol *root;
    AstNode *decl_stmt;
    XaOsResourceAlias *aliases;
    XaOsResourceLintFnSummary *fn_summaries;
    bool finalized;
    bool transferred;
    bool pipe_read_closed;
    bool pipe_write_closed;
    struct XaOsResourceLintState *next;
} XaOsResourceLintState;

typedef struct XaOsResourceLintSnapshot {
    bool finalized;
    bool transferred;
    bool pipe_read_closed;
    bool pipe_write_closed;
    XaOsResourceAlias *aliases;
} XaOsResourceLintSnapshot;

typedef struct XaOsResourceParamSummary {
    bool is_resource;
    XaOsResourceKind kind;
    bool finalized;
    bool pipe_read_closed;
    bool pipe_write_closed;
} XaOsResourceParamSummary;

struct XaOsResourceLintFnSummary {
    const char *name;
    uint32_t symbol_id;
    XaOsResourceParamSummary *params;
    int return_param_index;
    bool returns_new_resource;
    XaOsResourceKind return_kind;
    int param_count;
    struct XaOsResourceLintFnSummary *next;
};

static XaOsResourceLintState *
xa_os_resource_lint_find_returned_call_arg(XaOsResourceLintState *states, AstNode *expr);

static void xa_os_resource_lint_free_fn_summaries(XaOsResourceLintFnSummary *summaries) {
    while (summaries) {
        XaOsResourceLintFnSummary *next = summaries->next;
        xr_free(summaries->params);
        xr_free(summaries);
        summaries = next;
    }
}

typedef struct XaOsResourceNullCheck {
    uint32_t symbol_id;
    bool then_is_non_null;
} XaOsResourceNullCheck;

static const char *xa_os_resource_type_name(XaOsResourceKind kind) {
    return kind == XA_OS_RESOURCE_PIPE ? "Pipe" : "Process";
}

static const char *xa_os_resource_open_name(XaOsResourceKind kind) {
    return kind == XA_OS_RESOURCE_PIPE ? "sys.Pipe.open" : "sys.Process.spawn";
}

static const char *xa_os_resource_close_method(XaOsResourceKind kind) {
    return kind == XA_OS_RESOURCE_PIPE ? "close" : "wait";
}

static const char *xa_os_resource_close_past_tense(XaOsResourceKind kind) {
    return kind == XA_OS_RESOURCE_PIPE ? "closed" : "waited";
}

static bool xa_lifecycle_lint_os_resource_type_kind(XrType *type, XaOsResourceKind *kind_out) {
    if (!type)
        return false;
    if (xr_type_is_named_class(type, "Process")) {
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PROCESS;
        return true;
    }
    if (xr_type_is_named_class(type, "Pipe")) {
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PIPE;
        return true;
    }
    return false;
}

static bool xa_lifecycle_lint_function_param_os_resource_kind(XaInferContext *ctx,
                                                              const FunctionDeclNode *fn, int index,
                                                              XaOsResourceKind *kind_out) {
    if (xa_lifecycle_lint_os_resource_type_kind(
            xa_lifecycle_lint_function_param_type(ctx, fn, index), kind_out))
        return true;
    if (xa_lifecycle_lint_function_param_type_ref_named(fn, index, "Process")) {
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PROCESS;
        return true;
    }
    if (xa_lifecycle_lint_function_param_type_ref_named(fn, index, "Pipe")) {
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PIPE;
        return true;
    }
    return false;
}

static bool xa_symbol_is_local_os_resource_handle(XaSymbol *sym, XaScope *current_scope,
                                                  XaOsResourceKind kind) {
    (void) kind;
    (void) current_scope;
    if (!sym || sym->kind != XA_SYM_VARIABLE)
        return false;
    if (sym->is_shared || sym->is_exported || sym->is_imported)
        return false;
    return true;
}

static bool xa_member_path_is_sys_resource_namespace(AstNode *expr, const char *type_name) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_MEMBER_ACCESS || !type_name)
        return false;
    MemberAccessNode *type_member = &expr->as.member_access;
    if (!type_member->name || strcmp(type_member->name, type_name) != 0)
        return false;
    AstNode *ns = xa_thread_lint_unwrap_expr(type_member->object);
    return ns && ns->type == AST_VARIABLE && ns->as.variable.name &&
           strcmp(ns->as.variable.name, "sys") == 0;
}

static bool xa_symbol_is_sys_resource_class(XaInferContext *ctx, XaSymbol *sym,
                                            const char *type_name) {
    if (!ctx || !ctx->analyzer || !sym || !type_name ||
        (sym->kind != XA_SYM_CLASS && sym->kind != XA_SYM_IMPORT))
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    const char *class_name =
        (links && links->import_member_name) ? links->import_member_name : sym->name;
    if (!class_name || strcmp(class_name, type_name) != 0)
        return false;
    if (links && links->module_name && strcmp(links->module_name, "sys") == 0)
        return true;
    return links && links->file_path && strstr(links->file_path, "sys/sys.xr") != NULL;
}

static bool xa_expr_is_sys_resource_type_namespace(XaInferContext *ctx, AstNode *expr,
                                                   const char *type_name) {
    if (xa_member_path_is_sys_resource_namespace(expr, type_name))
        return true;
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!ctx || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
    XaSymbol *sym = xa_lookup_visible_symbol(ctx, expr->as.variable.name);
    return xa_symbol_is_sys_resource_class(ctx, sym, type_name);
}

static bool xa_process_options_arg_detached_literal(AstNode **args, int arg_count) {
    if (arg_count < 6 || !args || !args[5])
        return false;
    AstNode *detached = xa_thread_lint_unwrap_expr(args[5]);
    return detached && detached->type == AST_LITERAL_TRUE;
}

static bool xa_process_options_expr_has_detached_literal(AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;

    if (expr->type == AST_NEW_EXPR)
        return xa_process_options_arg_detached_literal(expr->as.new_expr.arguments,
                                                       expr->as.new_expr.arg_count);
    if (expr->type == AST_CALL_EXPR)
        return xa_process_options_arg_detached_literal(expr->as.call_expr.arguments,
                                                       expr->as.call_expr.arg_count);
    return false;
}

static bool xa_process_options_ctor_detached_literal(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || !xa_process_options_expr_has_detached_literal(expr))
        return false;

    XrType *expr_type =
        (ctx && ctx->analyzer) ? xa_analyzer_get_node_type(ctx->analyzer, expr) : NULL;
    const char *type_name = expr_type ? xr_type_get_class_name(expr_type) : NULL;
    if (type_name && strcmp(type_name, "ProcessOptions") == 0)
        return true;

    if (expr->type == AST_NEW_EXPR) {
        NewExprNode *ne = &expr->as.new_expr;
        return ne->class_name && strcmp(ne->class_name, "ProcessOptions") == 0 &&
               (!ne->module_name || strcmp(ne->module_name, "sys") == 0);
    }

    if (expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    bool is_options = false;
    if (callee && callee->type == AST_VARIABLE && callee->as.variable.name &&
        strcmp(callee->as.variable.name, "ProcessOptions") == 0) {
        is_options = true;
    } else if (callee && callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &callee->as.member_access;
        AstNode *ns = xa_thread_lint_unwrap_expr(ma->object);
        is_options = ma->name && strcmp(ma->name, "ProcessOptions") == 0 && ns &&
                     ns->type == AST_VARIABLE && ns->as.variable.name &&
                     strcmp(ns->as.variable.name, "sys") == 0;
    }
    return is_options;
}

static bool xa_process_spawn_detached_literal(XaInferContext *ctx, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    return call->arg_count >= 3 &&
           xa_process_options_ctor_detached_literal(ctx, call->arguments[2]);
}

static bool xa_expr_is_sys_os_resource_open_call(XaInferContext *ctx, AstNode *expr,
                                                 XaOsResourceKind *kind_out) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    if (ma->name && strcmp(ma->name, "spawn") == 0 &&
        xa_expr_is_sys_resource_type_namespace(ctx, ma->object, "Process")) {
        if (xa_process_spawn_detached_literal(ctx, expr))
            return false;
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PROCESS;
        return true;
    }
    if (ma->name && strcmp(ma->name, "open") == 0 &&
        xa_expr_is_sys_resource_type_namespace(ctx, ma->object, "Pipe")) {
        if (kind_out)
            *kind_out = XA_OS_RESOURCE_PIPE;
        return true;
    }
    return false;
}

static void xa_os_resource_lint_free_aliases(XaOsResourceAlias *aliases) {
    while (aliases) {
        XaOsResourceAlias *next = aliases->next;
        xr_free(aliases);
        aliases = next;
    }
}

static XaOsResourceAlias *xa_os_resource_lint_clone_aliases(XaOsResourceAlias *aliases) {
    XaOsResourceAlias *head = NULL;
    XaOsResourceAlias **tail = &head;
    for (XaOsResourceAlias *alias = aliases; alias; alias = alias->next) {
        XaOsResourceAlias *copy = xr_calloc(1, sizeof(XaOsResourceAlias));
        if (!copy)
            break;
        *copy = *alias;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

static void xa_os_resource_lint_free_states(XaOsResourceLintState *states) {
    while (states) {
        XaOsResourceLintState *next = states->next;
        xa_os_resource_lint_free_aliases(states->aliases);
        xr_free(states);
        states = next;
    }
}

static void xa_os_resource_lint_add_alias(XaOsResourceLintState *state, XaSymbol *sym) {
    if (!state || !sym || sym->id == 0)
        return;
    uint32_t symbol_id = sym->id;
    for (XaOsResourceAlias *a = state->aliases; a; a = a->next) {
        if (a->sym == sym || a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
            return;
    }
    XaOsResourceAlias *alias = xr_calloc(1, sizeof(XaOsResourceAlias));
    if (!alias)
        return;
    alias->sym = sym;
    alias->symbol_id = symbol_id;
    alias->name = sym->name;
    alias->next = state->aliases;
    state->aliases = alias;
}

static void xa_os_resource_lint_add_alias_id(XaOsResourceLintState *state, uint32_t symbol_id) {
    if (!state || symbol_id == 0)
        return;
    for (XaOsResourceAlias *a = state->aliases; a; a = a->next) {
        if (a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
            return;
    }
    XaOsResourceAlias *alias = xr_calloc(1, sizeof(XaOsResourceAlias));
    if (!alias)
        return;
    alias->symbol_id = symbol_id;
    alias->next = state->aliases;
    state->aliases = alias;
}

static void xa_os_resource_lint_add_alias_name(XaOsResourceLintState *state, const char *name) {
    if (!state || !name)
        return;
    for (XaOsResourceAlias *a = state->aliases; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0)
            return;
    }
    XaOsResourceAlias *alias = xr_calloc(1, sizeof(XaOsResourceAlias));
    if (!alias)
        return;
    alias->name = name;
    alias->next = state->aliases;
    state->aliases = alias;
}

static bool xa_os_resource_lint_alias_has_symbol_id(XaOsResourceAlias *alias, uint32_t symbol_id) {
    return alias && symbol_id != 0 &&
           (alias->symbol_id == symbol_id || (alias->sym && alias->sym->id == symbol_id));
}

static void xa_os_resource_lint_remove_alias_id(XaOsResourceLintState *states, uint32_t symbol_id) {
    if (symbol_id == 0)
        return;
    for (XaOsResourceLintState *s = states; s; s = s->next) {
        XaOsResourceAlias **link = &s->aliases;
        while (*link) {
            XaOsResourceAlias *alias = *link;
            if (xa_os_resource_lint_alias_has_symbol_id(alias, symbol_id)) {
                *link = alias->next;
                xr_free(alias);
                continue;
            }
            link = &alias->next;
        }
    }
}

static XaOsResourceLintState *xa_os_resource_lint_find_by_symbol_id(XaOsResourceLintState *states,
                                                                    uint32_t symbol_id) {
    if (symbol_id == 0)
        return NULL;
    for (XaOsResourceLintState *s = states; s; s = s->next) {
        for (XaOsResourceAlias *a = s->aliases; a; a = a->next) {
            if (a->symbol_id == symbol_id || (a->sym && a->sym->id == symbol_id))
                return s;
        }
    }
    return NULL;
}

static XaOsResourceLintState *xa_os_resource_lint_find_by_expr(XaOsResourceLintState *states,
                                                               AstNode *expr) {
    uint32_t symbol_id = xa_thread_lint_expr_symbol_id(expr);
    XaOsResourceLintState *state = xa_os_resource_lint_find_by_symbol_id(states, symbol_id);
    if (state)
        return state;
    const char *name = xa_thread_lint_expr_symbol_name(expr);
    if (!name)
        return NULL;
    for (XaOsResourceLintState *s = states; s; s = s->next) {
        if (s->root || s->decl_stmt)
            continue;
        for (XaOsResourceAlias *a = s->aliases; a; a = a->next) {
            if (a->name && strcmp(a->name, name) == 0)
                return s;
        }
    }
    return NULL;
}

static int xa_os_resource_lint_state_count(XaOsResourceLintState *states) {
    int count = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next)
        count++;
    return count;
}

static void xa_os_resource_lint_snapshot_states(XaOsResourceLintState *states,
                                                XaOsResourceLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        xa_os_resource_lint_free_aliases(snapshots[i].aliases);
        snapshots[i].finalized = s->finalized;
        snapshots[i].transferred = s->transferred;
        snapshots[i].pipe_read_closed = s->pipe_read_closed;
        snapshots[i].pipe_write_closed = s->pipe_write_closed;
        snapshots[i].aliases = xa_os_resource_lint_clone_aliases(s->aliases);
    }
}

static void xa_os_resource_lint_restore_states(XaOsResourceLintState *states,
                                               XaOsResourceLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        s->finalized = snapshots[i].finalized;
        s->transferred = snapshots[i].transferred;
        s->pipe_read_closed = snapshots[i].pipe_read_closed;
        s->pipe_write_closed = snapshots[i].pipe_write_closed;
        xa_os_resource_lint_free_aliases(s->aliases);
        s->aliases = xa_os_resource_lint_clone_aliases(snapshots[i].aliases);
    }
}

static void xa_os_resource_lint_clear_snapshot_array(XaOsResourceLintSnapshot *snapshots,
                                                     int count) {
    if (!snapshots)
        return;
    for (int i = 0; i < count; i++) {
        xa_os_resource_lint_free_aliases(snapshots[i].aliases);
        memset(&snapshots[i], 0, sizeof(snapshots[i]));
    }
}

static void xa_os_resource_lint_free_snapshot_array(XaOsResourceLintSnapshot *snapshots,
                                                    int count) {
    xa_os_resource_lint_clear_snapshot_array(snapshots, count);
    xr_free(snapshots);
}

static uint32_t xa_os_resource_lint_alias_key_id(XaOsResourceAlias *alias) {
    if (!alias)
        return 0;
    if (alias->symbol_id != 0)
        return alias->symbol_id;
    return alias->sym ? alias->sym->id : 0;
}

static bool xa_os_resource_lint_alias_same_key(XaOsResourceAlias *a, XaOsResourceAlias *b) {
    if (!a || !b)
        return false;
    uint32_t a_id = xa_os_resource_lint_alias_key_id(a);
    uint32_t b_id = xa_os_resource_lint_alias_key_id(b);
    if (a_id != 0 || b_id != 0)
        return a_id != 0 && a_id == b_id;
    if (a->sym || b->sym)
        return a->sym && a->sym == b->sym;
    return a->name && b->name && strcmp(a->name, b->name) == 0;
}

static void xa_os_resource_lint_remove_alias_like(XaOsResourceLintState *states,
                                                  XaOsResourceAlias *key) {
    if (!key)
        return;
    for (XaOsResourceLintState *s = states; s; s = s->next) {
        XaOsResourceAlias **link = &s->aliases;
        while (*link) {
            XaOsResourceAlias *alias = *link;
            if (xa_os_resource_lint_alias_same_key(alias, key)) {
                *link = alias->next;
                xr_free(alias);
                continue;
            }
            link = &alias->next;
        }
    }
}

static void xa_os_resource_lint_add_alias_copy(XaOsResourceLintState *state,
                                               XaOsResourceAlias *source) {
    if (!state || !source)
        return;
    for (XaOsResourceAlias *alias = state->aliases; alias; alias = alias->next) {
        if (xa_os_resource_lint_alias_same_key(alias, source))
            return;
    }
    XaOsResourceAlias *copy = xr_calloc(1, sizeof(XaOsResourceAlias));
    if (!copy)
        return;
    *copy = *source;
    copy->next = state->aliases;
    state->aliases = copy;
}

static XaOsResourceLintState *xa_os_resource_lint_state_at_index(XaOsResourceLintState *states,
                                                                 int index) {
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        if (i == index)
            return s;
    }
    return NULL;
}

static XaOsResourceAlias *
xa_os_resource_lint_snapshot_find_alias(XaOsResourceLintSnapshot *snapshots, int count,
                                        XaOsResourceAlias *key, int *source_index_out) {
    if (source_index_out)
        *source_index_out = -1;
    if (!snapshots || !key)
        return NULL;
    for (int i = 0; i < count; i++) {
        for (XaOsResourceAlias *alias = snapshots[i].aliases; alias; alias = alias->next) {
            if (xa_os_resource_lint_alias_same_key(alias, key)) {
                if (source_index_out)
                    *source_index_out = i;
                return alias;
            }
        }
    }
    return NULL;
}

static bool xa_os_resource_lint_alias_key_seen(XaOsResourceAlias *keys, XaOsResourceAlias *key) {
    for (XaOsResourceAlias *seen = keys; seen; seen = seen->next) {
        if (xa_os_resource_lint_alias_same_key(seen, key))
            return true;
    }
    return false;
}

static void xa_os_resource_lint_add_alias_merge_key(XaOsResourceAlias **keys,
                                                    XaOsResourceAlias *key) {
    if (!keys || !key || xa_os_resource_lint_alias_key_seen(*keys, key))
        return;
    XaOsResourceAlias *copy = xr_calloc(1, sizeof(XaOsResourceAlias));
    if (!copy)
        return;
    *copy = *key;
    copy->next = *keys;
    *keys = copy;
}

static void xa_os_resource_lint_collect_alias_merge_keys(XaOsResourceAlias **keys,
                                                         XaOsResourceLintSnapshot *snapshots,
                                                         int count) {
    if (!keys || !snapshots)
        return;
    for (int i = 0; i < count; i++) {
        for (XaOsResourceAlias *alias = snapshots[i].aliases; alias; alias = alias->next)
            xa_os_resource_lint_add_alias_merge_key(keys, alias);
    }
}

static void xa_os_resource_lint_merge_alias_snapshots(XaOsResourceLintState *states,
                                                      XaOsResourceLintSnapshot *before,
                                                      XaOsResourceLintSnapshot **paths,
                                                      int path_count, int state_count) {
    if (!states || !before || !paths || path_count <= 0 || state_count <= 0)
        return;

    XaOsResourceAlias *keys = NULL;
    xa_os_resource_lint_collect_alias_merge_keys(&keys, before, state_count);
    for (int pi = 0; pi < path_count; pi++)
        xa_os_resource_lint_collect_alias_merge_keys(&keys, paths[pi], state_count);

    for (XaOsResourceAlias *key = keys; key; key = key->next) {
        int stable_source = -1;
        XaOsResourceAlias *stable_alias = NULL;
        bool stable = true;
        for (int pi = 0; pi < path_count; pi++) {
            int source_index = -1;
            XaOsResourceAlias *alias =
                xa_os_resource_lint_snapshot_find_alias(paths[pi], state_count, key, &source_index);
            if (!alias || source_index < 0 ||
                (stable_source >= 0 && source_index != stable_source)) {
                stable = false;
                break;
            }
            if (stable_source < 0) {
                stable_source = source_index;
                stable_alias = alias;
            }
        }

        xa_os_resource_lint_remove_alias_like(states, key);
        if (!stable)
            continue;
        XaOsResourceLintState *target = xa_os_resource_lint_state_at_index(states, stable_source);
        xa_os_resource_lint_add_alias_copy(target, stable_alias ? stable_alias : key);
    }

    xa_os_resource_lint_free_aliases(keys);
}

static void xa_os_resource_lint_free_snapshot_paths(XaOsResourceLintSnapshot **paths,
                                                    int path_count, int state_count,
                                                    XaOsResourceLintSnapshot *borrowed_a,
                                                    XaOsResourceLintSnapshot *borrowed_b) {
    if (!paths)
        return;
    for (int i = 0; i < path_count; i++) {
        if (paths[i] && paths[i] != borrowed_a && paths[i] != borrowed_b)
            xa_os_resource_lint_free_snapshot_array(paths[i], state_count);
    }
    xr_free(paths);
}

static bool xa_os_resource_lint_snapshot_closed(XaOsResourceLintSnapshot *snapshot) {
    return snapshot && (snapshot->finalized || snapshot->transferred);
}

static bool xa_os_resource_lint_snapshot_pipe_read_closed(XaOsResourceLintSnapshot *snapshot) {
    return snapshot &&
           (snapshot->pipe_read_closed || xa_os_resource_lint_snapshot_closed(snapshot));
}

static bool xa_os_resource_lint_snapshot_pipe_write_closed(XaOsResourceLintSnapshot *snapshot) {
    return snapshot &&
           (snapshot->pipe_write_closed || xa_os_resource_lint_snapshot_closed(snapshot));
}

static bool xa_os_resource_lint_merge_pipe_side_snapshots(XaOsResourceLintState *states,
                                                          XaOsResourceLintSnapshot *before,
                                                          XaOsResourceLintSnapshot **paths,
                                                          int path_count, int state_count) {
    if (!states || !before || !paths || path_count <= 0 || state_count <= 0)
        return false;

    bool progressed = false;
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        if (i >= state_count || s->kind != XA_OS_RESOURCE_PIPE)
            continue;

        bool before_read = xa_os_resource_lint_snapshot_pipe_read_closed(&before[i]);
        bool before_write = xa_os_resource_lint_snapshot_pipe_write_closed(&before[i]);
        bool all_read = true;
        bool all_write = true;
        for (int pi = 0; pi < path_count; pi++) {
            XaOsResourceLintSnapshot *path = paths[pi];
            if (!path) {
                all_read = false;
                all_write = false;
                break;
            }
            all_read = all_read && xa_os_resource_lint_snapshot_pipe_read_closed(&path[i]);
            all_write = all_write && xa_os_resource_lint_snapshot_pipe_write_closed(&path[i]);
        }

        if (!before_read && all_read) {
            s->pipe_read_closed = true;
            progressed = true;
        }
        if (!before_write && all_write) {
            s->pipe_write_closed = true;
            progressed = true;
        }
        if (s->pipe_read_closed && s->pipe_write_closed && !s->finalized) {
            s->finalized = true;
            progressed = true;
        }
    }
    return progressed;
}

static void xa_os_resource_lint_mark_pipe_side_closed(XaOsResourceLintState *state,
                                                      const char *method_name) {
    if (!state || state->kind != XA_OS_RESOURCE_PIPE || !method_name)
        return;
    if (strcmp(method_name, "closeRead") == 0)
        state->pipe_read_closed = true;
    else if (strcmp(method_name, "closeWrite") == 0)
        state->pipe_write_closed = true;
    if (state->pipe_read_closed && state->pipe_write_closed)
        state->finalized = true;
}

static bool xa_os_resource_lint_null_check(AstNode *expr, XaOsResourceNullCheck *out) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || !out || (expr->type != AST_BINARY_EQ && expr->type != AST_BINARY_NE))
        return false;
    BinaryNode *bin = &expr->as.binary;
    AstNode *left = xa_thread_lint_unwrap_expr(bin->left);
    AstNode *right = xa_thread_lint_unwrap_expr(bin->right);
    uint32_t symbol_id = 0;
    if (left && left->type == AST_VARIABLE && right && right->type == AST_LITERAL_NULL)
        symbol_id = left->as.variable.symbol_id;
    else if (right && right->type == AST_VARIABLE && left && left->type == AST_LITERAL_NULL)
        symbol_id = right->as.variable.symbol_id;
    if (symbol_id == 0)
        return false;
    out->symbol_id = symbol_id;
    out->then_is_non_null = expr->type == AST_BINARY_NE;
    return true;
}

static void xa_os_resource_lint_scan_expr(XaOsResourceLintState *states, AstNode *expr,
                                          bool return_value, bool can_escape);
static void xa_os_resource_lint_scan_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                          bool can_escape);
static XaOsResourceLintState *
xa_os_resource_lint_find_returned_call_arg(XaOsResourceLintState *states, AstNode *expr);
static void xa_os_resource_lint_scan_ternary_expr(XaOsResourceLintState *states, AstNode *expr,
                                                  bool return_value, bool can_escape);
static void xa_os_resource_lint_scan_try_catch_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                                    bool can_escape);
static void xa_os_resource_lint_scan_match_expr(XaOsResourceLintState *states, AstNode *expr,
                                                bool can_escape);
static void xa_os_resource_lint_scan_select_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                                 bool can_escape);
static XaOsResourceLintState *xa_os_resource_lint_find_alias_source(XaOsResourceLintState *states,
                                                                    AstNode *expr);

static void xa_os_resource_lint_scan_expr_array(XaOsResourceLintState *states, AstNode **nodes,
                                                int count, bool return_value, bool can_escape) {
    if (!nodes || count <= 0)
        return;
    for (int i = 0; i < count; i++)
        xa_os_resource_lint_scan_expr(states, nodes[i], return_value, can_escape);
}

static XaOsResourceLintState *xa_os_resource_lint_find_alias_source(XaOsResourceLintState *states,
                                                                    AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr)
        return NULL;
    if (expr->type == AST_TERNARY) {
        XaOsResourceLintState *true_state =
            xa_os_resource_lint_find_alias_source(states, expr->as.ternary.true_expr);
        XaOsResourceLintState *false_state =
            xa_os_resource_lint_find_alias_source(states, expr->as.ternary.false_expr);
        return true_state && true_state == false_state ? true_state : NULL;
    }
    if (expr->type == AST_NULLISH_COALESCE) {
        XaOsResourceLintState *left_state =
            xa_os_resource_lint_find_alias_source(states, expr->as.binary.left);
        XaOsResourceLintState *right_state =
            xa_os_resource_lint_find_alias_source(states, expr->as.binary.right);
        return left_state && left_state == right_state ? left_state : NULL;
    }
    if (expr->type == AST_MATCH_EXPR) {
        MatchExprNode *match = &expr->as.match_expr;
        XaOsResourceLintState *matched_state = NULL;
        for (int i = 0; i < match->arm_count; i++) {
            AstNode *arm = match->arms ? match->arms[i] : NULL;
            if (!arm || arm->type != AST_MATCH_ARM)
                return NULL;
            AstNode *body = xa_thread_lint_unwrap_expr(arm->as.match_arm.body);
            if (body && (body->type == AST_BLOCK || body->type == AST_PROGRAM))
                body = xa_lifecycle_lint_single_expr_block_value(body);
            XaOsResourceLintState *arm_state = xa_os_resource_lint_find_alias_source(states, body);
            if (!arm_state || (matched_state && matched_state != arm_state))
                return NULL;
            matched_state = arm_state;
        }
        return matched_state;
    }
    if (expr->type == AST_MOVE_EXPR)
        return xa_os_resource_lint_find_alias_source(states, expr->as.move_expr.expr);
    if (expr->type == AST_AS_EXPR)
        return xa_os_resource_lint_find_alias_source(states, expr->as.as_expr.expr);
    if (expr->type == AST_UNSAFE_EXPR) {
        AstNode *operand = xa_thread_lint_unwrap_expr(expr->as.unsafe_expr.operand);
        if (!operand)
            return NULL;
        if (operand->type == AST_BLOCK || operand->type == AST_PROGRAM)
            operand = xa_lifecycle_lint_single_expr_block_value(operand);
        return xa_os_resource_lint_find_alias_source(states, operand);
    }
    XaOsResourceLintState *state = xa_os_resource_lint_find_by_expr(states, expr);
    if (!state)
        state = xa_os_resource_lint_find_returned_call_arg(states, expr);
    return state;
}

static void xa_os_resource_lint_note_var_alias(XaOsResourceLintState *states, VarDeclNode *var) {
    if (!states || !var || var->symbol_id == 0 || !var->initializer ||
        var->storage_mode != XR_STORAGE_NORMAL)
        return;
    XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, var->initializer);
    if (state)
        xa_os_resource_lint_add_alias_id(state, var->symbol_id);
}

static void xa_os_resource_lint_note_assignment_alias(XaOsResourceLintState *states,
                                                      AssignmentNode *assignment) {
    if (!states || !assignment || assignment->symbol_id == 0 || !assignment->value)
        return;
    XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, assignment->value);
    xa_os_resource_lint_remove_alias_id(states, assignment->symbol_id);
    if (state)
        xa_os_resource_lint_add_alias_id(state, assignment->symbol_id);
}

static void xa_os_resource_lint_note_destructure_aliases(XaOsResourceLintState *states,
                                                         XrDestructurePattern *pattern,
                                                         AstNode *initializer,
                                                         bool invalidate_targets) {
    if (!states || !pattern || !initializer)
        return;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER: {
            if (pattern->as.identifier.symbol_id == 0)
                return;
            XaOsResourceLintState *state =
                xa_os_resource_lint_find_alias_source(states, initializer);
            if (invalidate_targets)
                xa_os_resource_lint_remove_alias_id(states, pattern->as.identifier.symbol_id);
            if (state)
                xa_os_resource_lint_add_alias_id(state, pattern->as.identifier.symbol_id);
            return;
        }
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                xa_os_resource_lint_note_destructure_aliases(
                    states, pattern->as.array.elements[i],
                    xa_lifecycle_lint_destructure_source_at(initializer, i), invalidate_targets);
            }
            return;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                xa_os_resource_lint_note_destructure_aliases(
                    states, pattern->as.object.patterns[i],
                    xa_lifecycle_lint_object_source_for_field(initializer,
                                                              pattern->as.object.field_names[i]),
                    invalidate_targets);
            }
            return;
        default:
            return;
    }
}

static bool xa_os_resource_lint_scan_finalizer_call(XaOsResourceLintState *states, AstNode *expr,
                                                    bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, ma->object);
    if (!state || !ma->name)
        return false;
    if (strcmp(ma->name, xa_os_resource_close_method(state->kind)) == 0) {
        if (can_escape)
            state->finalized = true;
        return true;
    }
    if (state->kind == XA_OS_RESOURCE_PIPE &&
        (strcmp(ma->name, "closeRead") == 0 || strcmp(ma->name, "closeWrite") == 0)) {
        if (can_escape)
            xa_os_resource_lint_mark_pipe_side_closed(state, ma->name);
        return true;
    }
    return false;
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_find_fn_summary(XaInferContext *ctx, XaOsResourceLintFnSummary *summaries,
                                    AstNode *callee) {
    callee = xa_thread_lint_unwrap_expr(callee);
    if (!callee || callee->type != AST_VARIABLE || !callee->as.variable.name)
        return NULL;
    uint32_t symbol_id = xa_lifecycle_lint_callee_symbol_id(ctx, callee);
    for (XaOsResourceLintFnSummary *s = summaries; s; s = s->next) {
        if (!s->name || strcmp(s->name, callee->as.variable.name) != 0)
            continue;
        if (s->symbol_id != 0 && symbol_id != 0) {
            if (s->symbol_id == symbol_id)
                return s;
            continue;
        }
        if (s->symbol_id != 0 || symbol_id != 0)
            continue;
        return s;
    }
    return NULL;
}

static bool xa_os_resource_lint_scan_helper_finalizer_call(XaOsResourceLintState *states,
                                                           AstNode *expr, bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr || expr->type != AST_CALL_EXPR || !can_escape)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    XaOsResourceLintFnSummary *summary =
        xa_os_resource_lint_find_fn_summary(states->ctx, states->fn_summaries, call->callee);
    if (!summary || !summary->params)
        return false;
    bool applied = false;
    int n = call->arg_count < summary->param_count ? call->arg_count : summary->param_count;
    for (int i = 0; i < n; i++) {
        XaOsResourceParamSummary *param = &summary->params[i];
        if (!param->is_resource)
            continue;
        XaOsResourceLintState *state =
            xa_os_resource_lint_find_alias_source(states, call->arguments[i]);
        if (!state || state->kind != param->kind)
            continue;
        if (param->finalized)
            state->finalized = true;
        if (state->kind == XA_OS_RESOURCE_PIPE) {
            if (param->pipe_read_closed)
                state->pipe_read_closed = true;
            if (param->pipe_write_closed)
                state->pipe_write_closed = true;
            if (state->pipe_read_closed && state->pipe_write_closed)
                state->finalized = true;
        }
        applied = true;
    }
    return applied;
}

static bool xa_os_resource_lint_expr_returns_new_resource(XaInferContext *ctx,
                                                          XaOsResourceLintFnSummary *summaries,
                                                          AstNode *expr,
                                                          XaOsResourceKind *kind_out) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
    if (xa_expr_is_sys_os_resource_open_call(ctx, expr, &kind)) {
        if (kind_out)
            *kind_out = kind;
        return true;
    }
    switch (expr->type) {
        case AST_TERNARY: {
            XaOsResourceKind true_kind = XA_OS_RESOURCE_PROCESS;
            XaOsResourceKind false_kind = XA_OS_RESOURCE_PROCESS;
            if (!xa_os_resource_lint_expr_returns_new_resource(
                    ctx, summaries, expr->as.ternary.true_expr, &true_kind) ||
                !xa_os_resource_lint_expr_returns_new_resource(
                    ctx, summaries, expr->as.ternary.false_expr, &false_kind) ||
                true_kind != false_kind)
                return false;
            if (kind_out)
                *kind_out = true_kind;
            return true;
        }
        case AST_NULLISH_COALESCE: {
            XaOsResourceKind left_kind = XA_OS_RESOURCE_PROCESS;
            XaOsResourceKind right_kind = XA_OS_RESOURCE_PROCESS;
            if (!xa_os_resource_lint_expr_returns_new_resource(ctx, summaries, expr->as.binary.left,
                                                               &left_kind) ||
                !xa_os_resource_lint_expr_returns_new_resource(
                    ctx, summaries, expr->as.binary.right, &right_kind) ||
                left_kind != right_kind)
                return false;
            if (kind_out)
                *kind_out = left_kind;
            return true;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode *match = &expr->as.match_expr;
            if (match->arm_count <= 0)
                return false;
            XaOsResourceKind matched_kind = XA_OS_RESOURCE_PROCESS;
            bool have_kind = false;
            for (int i = 0; i < match->arm_count; i++) {
                AstNode *arm = match->arms ? match->arms[i] : NULL;
                if (!arm || arm->type != AST_MATCH_ARM)
                    return false;
                AstNode *body = xa_thread_lint_unwrap_expr(arm->as.match_arm.body);
                if (body && (body->type == AST_BLOCK || body->type == AST_PROGRAM))
                    body = xa_lifecycle_lint_single_expr_block_value(body);
                XaOsResourceKind arm_kind = XA_OS_RESOURCE_PROCESS;
                if (!xa_os_resource_lint_expr_returns_new_resource(ctx, summaries, body, &arm_kind))
                    return false;
                if (have_kind && arm_kind != matched_kind)
                    return false;
                matched_kind = arm_kind;
                have_kind = true;
            }
            if (kind_out)
                *kind_out = matched_kind;
            return true;
        }
        case AST_CALL_EXPR: {
            XaOsResourceLintFnSummary *summary =
                xa_os_resource_lint_find_fn_summary(ctx, summaries, expr->as.call_expr.callee);
            if (!summary || !summary->returns_new_resource)
                return false;
            if (kind_out)
                *kind_out = summary->return_kind;
            return true;
        }
        case AST_MOVE_EXPR:
            return xa_os_resource_lint_expr_returns_new_resource(ctx, summaries,
                                                                 expr->as.move_expr.expr, kind_out);
        case AST_AS_EXPR:
            return xa_os_resource_lint_expr_returns_new_resource(ctx, summaries,
                                                                 expr->as.as_expr.expr, kind_out);
        case AST_UNSAFE_EXPR: {
            AstNode *operand = xa_thread_lint_unwrap_expr(expr->as.unsafe_expr.operand);
            if (operand && (operand->type == AST_BLOCK || operand->type == AST_PROGRAM))
                operand = xa_lifecycle_lint_single_expr_block_value(operand);
            return xa_os_resource_lint_expr_returns_new_resource(ctx, summaries, operand, kind_out);
        }
        default:
            return false;
    }
}

static XaOsResourceLintState *
xa_os_resource_lint_find_returned_call_arg(XaOsResourceLintState *states, AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!states || !expr || expr->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &expr->as.call_expr;
    XaOsResourceLintFnSummary *summary =
        xa_os_resource_lint_find_fn_summary(states->ctx, states->fn_summaries, call->callee);
    if (!summary || !summary->params || summary->return_param_index < 0 ||
        summary->return_param_index >= summary->param_count ||
        summary->return_param_index >= call->arg_count)
        return NULL;
    XaOsResourceParamSummary *param = &summary->params[summary->return_param_index];
    if (!param->is_resource)
        return NULL;
    AstNode *arg = call->arguments[summary->return_param_index];
    XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, arg);
    return state && state->kind == param->kind ? state : NULL;
}

typedef struct XaOsResourceTryWaitResultAlias {
    uint32_t symbol_id;
    XaOsResourceLintState *state;
} XaOsResourceTryWaitResultAlias;

static XaOsResourceLintState *xa_os_resource_lint_try_wait_call_state(XaOsResourceLintState *states,
                                                                      AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || strcmp(ma->name, "tryWait") != 0)
        return NULL;
    XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, ma->object);
    if (!state || state->kind != XA_OS_RESOURCE_PROCESS)
        return NULL;
    return state;
}

static XaOsResourceLintState *
xa_os_resource_lint_find_try_wait_result_alias(XaOsResourceTryWaitResultAlias *aliases,
                                               int alias_count, uint32_t symbol_id) {
    if (!aliases || symbol_id == 0)
        return NULL;
    for (int i = 0; i < alias_count; i++) {
        if (aliases[i].symbol_id == symbol_id)
            return aliases[i].state;
    }
    return NULL;
}

static void
xa_os_resource_lint_remove_try_wait_result_alias(XaOsResourceTryWaitResultAlias *aliases,
                                                 int *alias_count, uint32_t symbol_id) {
    if (!aliases || !alias_count || symbol_id == 0)
        return;
    for (int i = 0; i < *alias_count; i++) {
        if (aliases[i].symbol_id == symbol_id) {
            aliases[i] = aliases[*alias_count - 1];
            (*alias_count)--;
            return;
        }
    }
}

static void xa_os_resource_lint_set_try_wait_result_alias(XaOsResourceTryWaitResultAlias *aliases,
                                                          int alias_capacity, int *alias_count,
                                                          uint32_t symbol_id,
                                                          XaOsResourceLintState *state) {
    if (!aliases || !alias_count || symbol_id == 0 || !state)
        return;
    xa_os_resource_lint_remove_try_wait_result_alias(aliases, alias_count, symbol_id);
    if (*alias_count >= alias_capacity)
        return;
    aliases[*alias_count].symbol_id = symbol_id;
    aliases[*alias_count].state = state;
    (*alias_count)++;
}

static void xa_os_resource_lint_note_try_wait_result_alias(XaOsResourceLintState *states,
                                                           AstNode *stmt,
                                                           XaOsResourceTryWaitResultAlias *aliases,
                                                           int alias_capacity, int *alias_count) {
    if (!states || !stmt || !aliases || !alias_count)
        return;
    uint32_t symbol_id = 0;
    AstNode *value = NULL;
    if (stmt->type == AST_VAR_DECL) {
        VarDeclNode *var = &stmt->as.var_decl;
        if (var->storage_mode != XR_STORAGE_NORMAL)
            return;
        symbol_id = var->symbol_id;
        value = var->initializer;
    } else if (stmt->type == AST_ASSIGNMENT) {
        AssignmentNode *assignment = &stmt->as.assignment;
        symbol_id = assignment->symbol_id;
        value = assignment->value;
        xa_os_resource_lint_remove_try_wait_result_alias(aliases, alias_count, symbol_id);
    } else {
        return;
    }
    if (symbol_id == 0)
        return;
    XaOsResourceLintState *state = xa_os_resource_lint_try_wait_call_state(states, value);
    if (state)
        xa_os_resource_lint_set_try_wait_result_alias(aliases, alias_capacity, alias_count,
                                                      symbol_id, state);
}

static XaOsResourceLintState *
xa_os_resource_lint_try_wait_result_state(XaOsResourceLintState *states, AstNode *expr,
                                          XaOsResourceTryWaitResultAlias *aliases,
                                          int alias_count) {
    expr = xa_thread_lint_unwrap_expr(expr);
    XaOsResourceLintState *state = xa_os_resource_lint_try_wait_call_state(states, expr);
    if (state)
        return state;
    if (expr && expr->type == AST_VARIABLE)
        return xa_os_resource_lint_find_try_wait_result_alias(aliases, alias_count,
                                                              expr->as.variable.symbol_id);
    return NULL;
}

static bool xa_os_resource_lint_try_wait_null_check(XaOsResourceLintState *states, AstNode *expr,
                                                    XaOsResourceTryWaitResultAlias *aliases,
                                                    int alias_count,
                                                    XaOsResourceLintState **state_out,
                                                    bool *then_is_reaped) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || (expr->type != AST_BINARY_EQ && expr->type != AST_BINARY_NE))
        return false;
    BinaryNode *bin = &expr->as.binary;
    AstNode *left = xa_thread_lint_unwrap_expr(bin->left);
    AstNode *right = xa_thread_lint_unwrap_expr(bin->right);
    XaOsResourceLintState *state = NULL;
    if (right && right->type == AST_LITERAL_NULL)
        state = xa_os_resource_lint_try_wait_result_state(states, left, aliases, alias_count);
    else if (left && left->type == AST_LITERAL_NULL)
        state = xa_os_resource_lint_try_wait_result_state(states, right, aliases, alias_count);
    if (!state)
        return false;
    if (state_out)
        *state_out = state;
    if (then_is_reaped)
        *then_is_reaped = expr->type == AST_BINARY_NE;
    return true;
}

static bool xa_os_resource_lint_try_wait_break_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                                    const char *loop_label,
                                                    XaOsResourceTryWaitResultAlias *aliases,
                                                    int alias_count,
                                                    XaOsResourceLintState **state_out) {
    if (!stmt || stmt->type != AST_IF_STMT)
        return false;
    XaOsResourceLintState *state = NULL;
    bool then_is_reaped = false;
    if (!xa_os_resource_lint_try_wait_null_check(states, stmt->as.if_stmt.condition, aliases,
                                                 alias_count, &state, &then_is_reaped))
        return false;
    AstNode *reaped_branch =
        then_is_reaped ? stmt->as.if_stmt.then_branch : stmt->as.if_stmt.else_branch;
    AstNode *running_branch =
        then_is_reaped ? stmt->as.if_stmt.else_branch : stmt->as.if_stmt.then_branch;
    if (!xa_lifecycle_lint_branch_tail_breaks_loop(reaped_branch, loop_label))
        return false;
    if (xa_lifecycle_lint_node_exits_current_scope(running_branch, loop_label))
        return false;
    if (state_out)
        *state_out = state;
    return true;
}

static bool xa_os_resource_lint_mark_try_wait_poll_loop(XaOsResourceLintState *states,
                                                        AstNode *stmt) {
    if (!states || !stmt || !xa_lifecycle_lint_loop_always_enters(states->ctx, stmt))
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(xa_lifecycle_lint_loop_body(stmt), &statements, &count) ||
        count <= 0)
        return false;

    XaOsResourceTryWaitResultAlias *aliases =
        xr_calloc((size_t) count, sizeof(XaOsResourceTryWaitResultAlias));
    if (!aliases)
        return false;
    int alias_count = 0;
    XaOsResourceLintState **found_states =
        xr_calloc((size_t) count, sizeof(XaOsResourceLintState *));
    if (!found_states) {
        xr_free(aliases);
        return false;
    }
    int found_count = 0;

    const char *loop_label = xa_lifecycle_lint_loop_label(stmt);
    bool found = false;
    for (int i = 0; i < count; i++) {
        xa_os_resource_lint_note_try_wait_result_alias(states, statements[i], aliases, count,
                                                       &alias_count);
        XaOsResourceLintState *state = NULL;
        if (xa_os_resource_lint_try_wait_break_stmt(states, statements[i], loop_label, aliases,
                                                    alias_count, &state)) {
            found = true;
            if (state && found_count < count)
                found_states[found_count++] = state;
            continue;
        }
        if (!found && xa_lifecycle_lint_node_skips_loop_tail(statements[i], loop_label)) {
            xr_free(found_states);
            xr_free(aliases);
            return false;
        }
        if (xa_lifecycle_lint_node_exits_current_scope(statements[i], loop_label)) {
            xr_free(found_states);
            xr_free(aliases);
            return false;
        }
    }
    if (!found) {
        xr_free(found_states);
        xr_free(aliases);
        return false;
    }
    for (int i = 0; i < found_count; i++)
        found_states[i]->finalized = true;
    xr_free(found_states);
    xr_free(aliases);
    return true;
}

static bool xa_os_resource_lint_mark_linear_finalizer_break_loop(XaOsResourceLintState *states,
                                                                 AstNode *stmt) {
    if (!states || !stmt || !xa_lifecycle_lint_loop_always_enters(states->ctx, stmt))
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(xa_lifecycle_lint_loop_body(stmt), &statements, &count) ||
        count <= 0)
        return false;

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return false;
    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(after, state_count);
        return false;
    }

    const char *loop_label = xa_lifecycle_lint_loop_label(stmt);
    xa_os_resource_lint_snapshot_states(states, before);
    for (int i = 0; i < count; i++) {
        AstNode *child = statements[i];
        if (xa_lifecycle_lint_branch_is_loop_break(child, loop_label)) {
            xa_os_resource_lint_snapshot_states(states, after);
            bool closed = false;
            for (int si = 0; si < state_count; si++) {
                if (!xa_os_resource_lint_snapshot_closed(&before[si]) &&
                    xa_os_resource_lint_snapshot_closed(&after[si])) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                xa_os_resource_lint_restore_states(states, before);
            XaOsResourceLintSnapshot *paths[] = {after};
            bool partial = false;
            if (!closed) {
                partial = xa_os_resource_lint_merge_pipe_side_snapshots(states, before, paths, 1,
                                                                        state_count);
            }
            xa_os_resource_lint_free_snapshot_array(before, state_count);
            xa_os_resource_lint_free_snapshot_array(after, state_count);
            return closed || partial;
        }
        if (xa_lifecycle_lint_node_skips_loop_tail(child, loop_label)) {
            xa_os_resource_lint_restore_states(states, before);
            xa_os_resource_lint_free_snapshot_array(before, state_count);
            xa_os_resource_lint_free_snapshot_array(after, state_count);
            return false;
        }
        xa_os_resource_lint_scan_stmt(states, child, true);
    }

    xa_os_resource_lint_restore_states(states, before);
    XaOsResourceLintSnapshot *paths[] = {after};
    bool partial =
        xa_os_resource_lint_merge_pipe_side_snapshots(states, before, paths, 1, state_count);
    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(after, state_count);
    return partial;
}

static bool xa_os_resource_lint_mark_nonempty_for_in_finalizer_loop(XaOsResourceLintState *states,
                                                                    AstNode *stmt) {
    if (!states || !xa_lifecycle_lint_for_in_always_enters(states->ctx, stmt))
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(stmt->as.for_in_stmt.body, &statements, &count) || count <= 0)
        return false;

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return false;
    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(after, state_count);
        return false;
    }

    const char *loop_label = stmt->as.for_in_stmt.label;
    xa_os_resource_lint_snapshot_states(states, before);
    for (int i = 0; i < count; i++) {
        AstNode *child = statements[i];
        if (xa_lifecycle_lint_node_skips_loop_tail(child, loop_label)) {
            xa_os_resource_lint_snapshot_states(states, after);
            bool closed = false;
            for (int si = 0; si < state_count; si++) {
                if (!xa_os_resource_lint_snapshot_closed(&before[si]) &&
                    xa_os_resource_lint_snapshot_closed(&after[si])) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                xa_os_resource_lint_restore_states(states, before);
            XaOsResourceLintSnapshot *paths[] = {after};
            bool partial = false;
            if (!closed) {
                partial = xa_os_resource_lint_merge_pipe_side_snapshots(states, before, paths, 1,
                                                                        state_count);
            }
            xa_os_resource_lint_free_snapshot_array(before, state_count);
            xa_os_resource_lint_free_snapshot_array(after, state_count);
            return closed || partial;
        }
        xa_os_resource_lint_scan_stmt(states, child, true);
    }

    xa_os_resource_lint_snapshot_states(states, after);
    bool closed = false;
    for (int si = 0; si < state_count; si++) {
        if (!xa_os_resource_lint_snapshot_closed(&before[si]) &&
            xa_os_resource_lint_snapshot_closed(&after[si])) {
            closed = true;
            break;
        }
    }
    if (!closed)
        xa_os_resource_lint_restore_states(states, before);
    XaOsResourceLintSnapshot *paths[] = {after};
    bool partial = false;
    if (!closed) {
        partial =
            xa_os_resource_lint_merge_pipe_side_snapshots(states, before, paths, 1, state_count);
    }
    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(after, state_count);
    return closed || partial;
}

static void xa_os_resource_lint_scan_defer_expr(XaOsResourceLintState *states, AstNode *expr,
                                                bool can_escape) {
    if (!states || !expr || !can_escape)
        return;

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;
    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *after = xr_calloc(1, snapshot_size);
    if (!before || !after) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(after, state_count);
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    AstNode *deferred = xa_thread_lint_unwrap_expr(expr);
    if (deferred && deferred->type == AST_FUNCTION_EXPR)
        xa_os_resource_lint_scan_stmt(states, deferred->as.function_expr.body, true);
    else
        xa_os_resource_lint_scan_expr(states, deferred, false, true);
    xa_os_resource_lint_snapshot_states(states, after);

    xa_os_resource_lint_restore_states(states, before);
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        if (after[i].pipe_read_closed)
            s->pipe_read_closed = true;
        if (after[i].pipe_write_closed)
            s->pipe_write_closed = true;
        if (!xa_os_resource_lint_snapshot_closed(&before[i]) && after[i].finalized)
            s->finalized = true;
        if (s->kind == XA_OS_RESOURCE_PIPE && s->pipe_read_closed && s->pipe_write_closed)
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(after, state_count);
}

static void xa_os_resource_lint_scan_expr(XaOsResourceLintState *states, AstNode *expr,
                                          bool return_value, bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return;

    if (return_value && can_escape) {
        XaOsResourceLintState *state = xa_os_resource_lint_find_alias_source(states, expr);
        if (state) {
            state->transferred = true;
            return;
        }
    }

    switch (expr->type) {
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return;
        case AST_CALL_EXPR: {
            if (xa_os_resource_lint_scan_finalizer_call(states, expr, can_escape))
                return;
            xa_os_resource_lint_scan_helper_finalizer_call(states, expr, can_escape);
            CallExprNode *call = &expr->as.call_expr;
            xa_os_resource_lint_scan_expr(states, call->callee, false, can_escape);
            xa_os_resource_lint_scan_expr_array(states, call->arguments, call->arg_count, false,
                                                can_escape);
            return;
        }
        case AST_MEMBER_ACCESS:
            xa_os_resource_lint_scan_expr(states, expr->as.member_access.object, false, can_escape);
            return;
        case AST_MEMBER_SET:
            xa_os_resource_lint_scan_expr(states, expr->as.member_set.object, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.member_set.value, false, can_escape);
            return;
        case AST_INDEX_GET:
            xa_os_resource_lint_scan_expr(states, expr->as.index_get.array, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.index_get.index, false, can_escape);
            return;
        case AST_INDEX_SET:
            xa_os_resource_lint_scan_expr(states, expr->as.index_set.array, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.index_set.index, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.index_set.value, false, can_escape);
            return;
        case AST_SLICE_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.slice_expr.source, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.slice_expr.start, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.slice_expr.end, false, can_escape);
            return;
        case AST_FORCE_UNWRAP:
            xa_os_resource_lint_scan_expr(states, expr->as.unary.operand, false, can_escape);
            return;
        case AST_GROUPING:
            xa_os_resource_lint_scan_expr(states, expr->as.grouping, false, can_escape);
            return;
        case AST_ASSIGNMENT:
            xa_os_resource_lint_note_assignment_alias(states, &expr->as.assignment);
            xa_os_resource_lint_scan_expr(states, expr->as.assignment.value, false, can_escape);
            return;
        case AST_COMPOUND_ASSIGNMENT:
            xa_os_resource_lint_scan_expr(states, expr->as.compound_assignment.object, false,
                                          can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.compound_assignment.value, false,
                                          can_escape);
            return;
        case AST_INC:
        case AST_DEC:
            return;
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
        case AST_NULLISH_COALESCE:
            xa_os_resource_lint_scan_expr(states, expr->as.binary.left, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.binary.right, false, can_escape);
            return;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_os_resource_lint_scan_expr(states, expr->as.unary.operand, false, can_escape);
            return;
        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat) {
                xa_os_resource_lint_scan_expr(states, expr->as.array_literal.repeat_value, false,
                                              can_escape);
                xa_os_resource_lint_scan_expr(states, expr->as.array_literal.repeat_count, false,
                                              can_escape);
            } else {
                xa_os_resource_lint_scan_expr_array(states, expr->as.array_literal.elements,
                                                    expr->as.array_literal.count, false,
                                                    can_escape);
            }
            return;
        case AST_TUPLE_LITERAL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.tuple_literal.elements,
                                                expr->as.tuple_literal.count, false, can_escape);
            return;
        case AST_TEMPLATE_STRING:
            xa_os_resource_lint_scan_expr_array(states, expr->as.template_str.parts,
                                                expr->as.template_str.part_count, false,
                                                can_escape);
            return;
        case AST_SPREAD_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.spread_expr.expr, false, can_escape);
            return;
        case AST_OBJECT_LITERAL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.object_literal.keys,
                                                expr->as.object_literal.count, false, can_escape);
            xa_os_resource_lint_scan_expr_array(states, expr->as.object_literal.values,
                                                expr->as.object_literal.count, false, can_escape);
            return;
        case AST_MAP_LITERAL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.map_literal.keys,
                                                expr->as.map_literal.count, false, can_escape);
            xa_os_resource_lint_scan_expr_array(states, expr->as.map_literal.values,
                                                expr->as.map_literal.count, false, can_escape);
            return;
        case AST_SET_LITERAL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.set_literal.elements,
                                                expr->as.set_literal.count, false, can_escape);
            return;
        case AST_STRUCT_LITERAL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.struct_literal.field_values,
                                                expr->as.struct_literal.field_count, false,
                                                can_escape);
            return;
        case AST_TERNARY:
            xa_os_resource_lint_scan_ternary_expr(states, expr, return_value, can_escape);
            return;
        case AST_OPTIONAL_CHAIN:
            xa_os_resource_lint_scan_expr(states, expr->as.optional_chain.object, false,
                                          can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.optional_chain.index, false, can_escape);
            return;
        case AST_RANGE:
            xa_os_resource_lint_scan_expr(states, expr->as.range.start, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.range.end, false, can_escape);
            return;
        case AST_IS_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.is_expr.expr, false, can_escape);
            return;
        case AST_AS_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.as_expr.expr, false, can_escape);
            return;
        case AST_COMPTIME_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.comptime_expr.expr, false, can_escape);
            return;
        case AST_UNSAFE_EXPR:
            if (expr->as.unsafe_expr.operand &&
                (expr->as.unsafe_expr.operand->type == AST_BLOCK ||
                 expr->as.unsafe_expr.operand->type == AST_PROGRAM)) {
                xa_os_resource_lint_scan_stmt(states, expr->as.unsafe_expr.operand, can_escape);
            } else {
                xa_os_resource_lint_scan_expr(states, expr->as.unsafe_expr.operand, false,
                                              can_escape);
            }
            return;
        case AST_MOVE_EXPR: {
            xa_os_resource_lint_scan_expr(states, expr->as.move_expr.expr, false, can_escape);
            return;
        }
        case AST_MATCH_EXPR:
            xa_os_resource_lint_scan_match_expr(states, expr, can_escape);
            return;
        case AST_GO_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.go_expr.expr, false, can_escape);
            return;
        case AST_AWAIT_EXPR:
            xa_os_resource_lint_scan_expr(states, expr->as.await_expr.expr, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.await_expr.timeout, false, can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.await_expr.into, false, can_escape);
            return;
        case AST_CHANNEL_NEW:
            xa_os_resource_lint_scan_expr(states, expr->as.channel_new.buffer_size, false,
                                          can_escape);
            return;
        case AST_NEW_EXPR:
            xa_os_resource_lint_scan_expr_array(states, expr->as.new_expr.arguments,
                                                expr->as.new_expr.arg_count, false, can_escape);
            return;
        case AST_SUPER_CALL:
            xa_os_resource_lint_scan_expr_array(states, expr->as.super_call.arguments,
                                                expr->as.super_call.arg_count, false, can_escape);
            return;
        case AST_ENUM_INDEX:
            xa_os_resource_lint_scan_expr(states, expr->as.enum_index.collection, false,
                                          can_escape);
            xa_os_resource_lint_scan_expr(states, expr->as.enum_index.index_expr, false,
                                          can_escape);
            return;
        case AST_YIELD_STMT:
            xa_os_resource_lint_scan_expr(states, expr->as.yield_stmt.value, false, can_escape);
            return;
        default:
            return;
    }
}

static void xa_os_resource_lint_scan_ternary_expr(XaOsResourceLintState *states, AstNode *expr,
                                                  bool return_value, bool can_escape) {
    if (!expr || expr->type != AST_TERNARY)
        return;
    TernaryNode *ternary = &expr->as.ternary;
    xa_os_resource_lint_scan_expr(states, ternary->condition, false, can_escape);

    if (!can_escape) {
        xa_os_resource_lint_scan_expr(states, ternary->true_expr, return_value, false);
        xa_os_resource_lint_scan_expr(states, ternary->false_expr, return_value, false);
        return;
    }

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *true_after = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *false_after = xr_calloc(1, snapshot_size);
    if (!before || !true_after || !false_after) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(true_after, state_count);
        xa_os_resource_lint_free_snapshot_array(false_after, state_count);
        xa_os_resource_lint_scan_expr(states, ternary->true_expr, return_value, false);
        xa_os_resource_lint_scan_expr(states, ternary->false_expr, return_value, false);
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    xa_os_resource_lint_scan_expr(states, ternary->true_expr, return_value, true);
    xa_os_resource_lint_snapshot_states(states, true_after);

    xa_os_resource_lint_restore_states(states, before);
    xa_os_resource_lint_scan_expr(states, ternary->false_expr, return_value, true);
    xa_os_resource_lint_snapshot_states(states, false_after);

    xa_os_resource_lint_restore_states(states, before);
    XaOsResourceLintSnapshot *alias_paths[] = {true_after, false_after};
    xa_os_resource_lint_merge_alias_snapshots(states, before, alias_paths, 2, state_count);
    xa_os_resource_lint_merge_pipe_side_snapshots(states, before, alias_paths, 2, state_count);
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_os_resource_lint_snapshot_closed(&before[i]);
        bool true_closed = xa_os_resource_lint_snapshot_closed(&true_after[i]);
        bool false_closed = xa_os_resource_lint_snapshot_closed(&false_after[i]);
        if (!before_closed && true_closed && false_closed)
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(true_after, state_count);
    xa_os_resource_lint_free_snapshot_array(false_after, state_count);
}

static void xa_os_resource_lint_scan_block(XaOsResourceLintState *states, AstNode *block,
                                           bool can_escape) {
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block, &statements, &count))
        return;
    for (int i = 0; i < count; i++)
        xa_os_resource_lint_scan_stmt(states, statements[i], can_escape);
}

static void xa_os_resource_lint_scan_if_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                             bool can_escape) {
    XaOsResourceNullCheck null_check = {0};
    bool has_null_check = xa_os_resource_lint_null_check(stmt->as.if_stmt.condition, &null_check);
    xa_os_resource_lint_scan_expr(states, stmt->as.if_stmt.condition, false, can_escape);
    if (!can_escape) {
        xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *then_after = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *else_after = xr_calloc(1, snapshot_size);
    if (!before || !then_after || !else_after) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(then_after, state_count);
        xa_os_resource_lint_free_snapshot_array(else_after, state_count);
        xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, true);
    xa_os_resource_lint_snapshot_states(states, then_after);

    xa_os_resource_lint_restore_states(states, before);
    if (stmt->as.if_stmt.else_branch)
        xa_os_resource_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, true);
    xa_os_resource_lint_snapshot_states(states, else_after);

    xa_os_resource_lint_restore_states(states, before);
    XaOsResourceLintSnapshot *alias_paths[] = {then_after, else_after};
    xa_os_resource_lint_merge_alias_snapshots(states, before, alias_paths, 2, state_count);
    xa_os_resource_lint_merge_pipe_side_snapshots(states, before, alias_paths, 2, state_count);
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_os_resource_lint_snapshot_closed(&before[i]);
        bool then_closed = xa_os_resource_lint_snapshot_closed(&then_after[i]);
        bool else_closed = xa_os_resource_lint_snapshot_closed(&else_after[i]);
        if (has_null_check && null_check.symbol_id != 0 &&
            xa_os_resource_lint_find_by_symbol_id(s, null_check.symbol_id) == s) {
            if (null_check.then_is_non_null)
                else_closed = true;
            else
                then_closed = true;
        }
        if (!before_closed && then_closed && else_closed)
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(then_after, state_count);
    xa_os_resource_lint_free_snapshot_array(else_after, state_count);
}

static void xa_os_resource_lint_scan_try_catch_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                                    bool can_escape) {
    if (!stmt || stmt->type != AST_TRY_CATCH)
        return;
    TryCatchNode *tc = &stmt->as.try_catch;

    if (!can_escape) {
        xa_os_resource_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_os_resource_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *try_after = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *catch_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !try_after || !catch_after || !all_paths_closed) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(try_after, state_count);
        xa_os_resource_lint_free_snapshot_array(catch_after, state_count);
        xr_free(all_paths_closed);
        xa_os_resource_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_os_resource_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    xa_os_resource_lint_scan_stmt(states, tc->try_body, true);
    xa_os_resource_lint_snapshot_states(states, try_after);

    int alias_path_count = 1 + tc->catch_count;
    XaOsResourceLintSnapshot **alias_paths =
        alias_path_count > 0 ? xr_calloc((size_t) alias_path_count, sizeof(*alias_paths)) : NULL;
    bool alias_paths_complete = alias_paths != NULL;
    if (alias_paths)
        alias_paths[0] = try_after;

    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = xa_os_resource_lint_snapshot_closed(&try_after[i]);

    for (int ci = 0; ci < tc->catch_count; ci++) {
        XrCatchClause *cc = tc->catch_clauses[ci];
        if (!cc) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_os_resource_lint_restore_states(states, before);
        XaOsResourceLintSnapshot *path_after = catch_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ci + 1] = path_after;
            else {
                alias_paths_complete = false;
                path_after = catch_after;
            }
        }
        if (path_after == catch_after)
            xa_os_resource_lint_clear_snapshot_array(catch_after, state_count);
        xa_os_resource_lint_scan_stmt(states, cc->body, true);
        xa_os_resource_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_os_resource_lint_snapshot_closed(&path_after[i]);
    }

    xa_os_resource_lint_restore_states(states, before);
    if (alias_paths_complete) {
        xa_os_resource_lint_merge_alias_snapshots(states, before, alias_paths, alias_path_count,
                                                  state_count);
        xa_os_resource_lint_merge_pipe_side_snapshots(states, before, alias_paths, alias_path_count,
                                                      state_count);
    }
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_os_resource_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_paths(alias_paths, alias_path_count, state_count, try_after,
                                            catch_after);
    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(try_after, state_count);
    xa_os_resource_lint_free_snapshot_array(catch_after, state_count);
    xr_free(all_paths_closed);
}

static void xa_os_resource_lint_scan_match_expr(XaOsResourceLintState *states, AstNode *expr,
                                                bool can_escape) {
    if (!expr || expr->type != AST_MATCH_EXPR)
        return;
    MatchExprNode *match = &expr->as.match_expr;
    xa_os_resource_lint_scan_expr(states, match->expr, false, can_escape);

    if (!can_escape) {
        for (int i = 0; i < match->arm_count; i++)
            xa_os_resource_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *arm_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !arm_after || !all_paths_closed) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(arm_after, state_count);
        xr_free(all_paths_closed);
        for (int i = 0; i < match->arm_count; i++)
            xa_os_resource_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = match->arm_count > 0;

    XaOsResourceLintSnapshot **alias_paths =
        match->arm_count > 0 ? xr_calloc((size_t) match->arm_count, sizeof(*alias_paths)) : NULL;
    bool alias_paths_complete = match->arm_count > 0 && alias_paths != NULL;

    for (int ai = 0; ai < match->arm_count; ai++) {
        if (!match->arms[ai]) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_os_resource_lint_restore_states(states, before);
        XaOsResourceLintSnapshot *path_after = arm_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ai] = path_after;
            else {
                alias_paths_complete = false;
                path_after = arm_after;
            }
        }
        if (path_after == arm_after)
            xa_os_resource_lint_clear_snapshot_array(arm_after, state_count);
        xa_os_resource_lint_scan_stmt(states, match->arms[ai], true);
        xa_os_resource_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_os_resource_lint_snapshot_closed(&path_after[i]);
    }

    xa_os_resource_lint_restore_states(states, before);
    if (alias_paths_complete) {
        xa_os_resource_lint_merge_alias_snapshots(states, before, alias_paths, match->arm_count,
                                                  state_count);
        xa_os_resource_lint_merge_pipe_side_snapshots(states, before, alias_paths, match->arm_count,
                                                      state_count);
    }
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_os_resource_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_paths(alias_paths, match->arm_count, state_count, arm_after,
                                            NULL);
    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(arm_after, state_count);
    xr_free(all_paths_closed);
}

static void xa_os_resource_lint_scan_select_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                                 bool can_escape) {
    if (!stmt || stmt->type != AST_SELECT_STMT)
        return;
    SelectStmtNode *select = &stmt->as.select_stmt;

    if (!can_escape) {
        for (int i = 0; i < select->case_count; i++)
            xa_os_resource_lint_scan_stmt(states, select->cases[i], false);
        return;
    }

    int state_count = xa_os_resource_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaOsResourceLintSnapshot) * (size_t) state_count;
    XaOsResourceLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaOsResourceLintSnapshot *case_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !case_after || !all_paths_closed) {
        xa_os_resource_lint_free_snapshot_array(before, state_count);
        xa_os_resource_lint_free_snapshot_array(case_after, state_count);
        xr_free(all_paths_closed);
        for (int i = 0; i < select->case_count; i++)
            xa_os_resource_lint_scan_stmt(states, select->cases[i], false);
        return;
    }

    xa_os_resource_lint_snapshot_states(states, before);
    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = select->case_count > 0;

    XaOsResourceLintSnapshot **alias_paths =
        select->case_count > 0 ? xr_calloc((size_t) select->case_count, sizeof(*alias_paths))
                               : NULL;
    bool alias_paths_complete = select->case_count > 0 && alias_paths != NULL;

    for (int ci = 0; ci < select->case_count; ci++) {
        if (!select->cases[ci]) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            alias_paths_complete = false;
            continue;
        }
        xa_os_resource_lint_restore_states(states, before);
        XaOsResourceLintSnapshot *path_after = case_after;
        if (alias_paths) {
            path_after = xr_calloc(1, snapshot_size);
            if (path_after)
                alias_paths[ci] = path_after;
            else {
                alias_paths_complete = false;
                path_after = case_after;
            }
        }
        if (path_after == case_after)
            xa_os_resource_lint_clear_snapshot_array(case_after, state_count);
        xa_os_resource_lint_scan_stmt(states, select->cases[ci], true);
        xa_os_resource_lint_snapshot_states(states, path_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_os_resource_lint_snapshot_closed(&path_after[i]);
    }

    xa_os_resource_lint_restore_states(states, before);
    if (alias_paths_complete) {
        xa_os_resource_lint_merge_alias_snapshots(states, before, alias_paths, select->case_count,
                                                  state_count);
        xa_os_resource_lint_merge_pipe_side_snapshots(states, before, alias_paths,
                                                      select->case_count, state_count);
    }
    int i = 0;
    for (XaOsResourceLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_os_resource_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xa_os_resource_lint_free_snapshot_paths(alias_paths, select->case_count, state_count,
                                            case_after, NULL);
    xa_os_resource_lint_free_snapshot_array(before, state_count);
    xa_os_resource_lint_free_snapshot_array(case_after, state_count);
    xr_free(all_paths_closed);
}

static void xa_os_resource_lint_scan_stmt(XaOsResourceLintState *states, AstNode *stmt,
                                          bool can_escape) {
    if (!stmt)
        return;
    switch (stmt->type) {
        case AST_EXPR_STMT:
            xa_os_resource_lint_scan_expr(states, stmt->as.expr_stmt, false, can_escape);
            return;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            xa_os_resource_lint_note_var_alias(states, &stmt->as.var_decl);
            xa_os_resource_lint_scan_expr(states, stmt->as.var_decl.initializer, false, can_escape);
            return;
        case AST_DESTRUCTURE_DECL:
            xa_os_resource_lint_note_destructure_aliases(states, stmt->as.destructure_decl.pattern,
                                                         stmt->as.destructure_decl.initializer,
                                                         false);
            xa_os_resource_lint_scan_expr(states, stmt->as.destructure_decl.initializer, false,
                                          can_escape);
            return;
        case AST_DESTRUCTURE_ASSIGN:
            xa_os_resource_lint_note_destructure_aliases(states,
                                                         stmt->as.destructure_assign.pattern,
                                                         stmt->as.destructure_assign.value, true);
            xa_os_resource_lint_scan_expr(states, stmt->as.destructure_assign.value, false,
                                          can_escape);
            return;
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
            xa_os_resource_lint_scan_expr(states, stmt, false, can_escape);
            return;
        case AST_PRINT_STMT:
            xa_os_resource_lint_scan_expr_array(states, stmt->as.print_stmt.exprs,
                                                stmt->as.print_stmt.expr_count, false, can_escape);
            return;
        case AST_RETURN_STMT:
            xa_os_resource_lint_scan_expr_array(states, stmt->as.return_stmt.values,
                                                stmt->as.return_stmt.value_count, true, can_escape);
            return;
        case AST_IF_STMT:
            xa_os_resource_lint_scan_if_stmt(states, stmt, can_escape);
            return;
        case AST_WHILE_STMT:
            xa_os_resource_lint_scan_expr(states, stmt->as.while_stmt.condition, false, can_escape);
            xa_os_resource_lint_scan_stmt(states, stmt->as.while_stmt.body, false);
            if (can_escape) {
                xa_os_resource_lint_mark_linear_finalizer_break_loop(states, stmt);
                xa_os_resource_lint_mark_try_wait_poll_loop(states, stmt);
            }
            return;
        case AST_FOR_STMT:
            xa_os_resource_lint_scan_stmt(states, stmt->as.for_stmt.initializer, can_escape);
            xa_os_resource_lint_scan_expr(states, stmt->as.for_stmt.condition, false, can_escape);
            xa_os_resource_lint_scan_expr(states, stmt->as.for_stmt.increment, false, false);
            xa_os_resource_lint_scan_stmt(states, stmt->as.for_stmt.body, false);
            if (can_escape) {
                xa_os_resource_lint_mark_linear_finalizer_break_loop(states, stmt);
                xa_os_resource_lint_mark_try_wait_poll_loop(states, stmt);
            }
            return;
        case AST_FOR_IN_STMT:
            xa_os_resource_lint_scan_expr(states, stmt->as.for_in_stmt.collection, false,
                                          can_escape);
            xa_os_resource_lint_scan_stmt(states, stmt->as.for_in_stmt.body, false);
            if (can_escape)
                xa_os_resource_lint_mark_nonempty_for_in_finalizer_loop(states, stmt);
            return;
        case AST_BLOCK:
        case AST_PROGRAM:
            xa_os_resource_lint_scan_block(states, stmt, can_escape);
            return;
        case AST_TRY_CATCH:
            xa_os_resource_lint_scan_try_catch_stmt(states, stmt, can_escape);
            return;
        case AST_MATCH_EXPR:
            xa_os_resource_lint_scan_match_expr(states, stmt, can_escape);
            return;
        case AST_MATCH_ARM:
            xa_os_resource_lint_scan_expr(states, stmt->as.match_arm.pattern, false, can_escape);
            xa_os_resource_lint_scan_expr(states, stmt->as.match_arm.guard, false, can_escape);
            xa_os_resource_lint_scan_stmt(states, stmt->as.match_arm.body, can_escape);
            return;
        case AST_SELECT_STMT:
            xa_os_resource_lint_scan_select_stmt(states, stmt, can_escape);
            return;
        case AST_SELECT_CASE:
            xa_os_resource_lint_scan_expr(states, stmt->as.select_case.channel, false, can_escape);
            xa_os_resource_lint_scan_expr(states, stmt->as.select_case.value, false, can_escape);
            xa_os_resource_lint_scan_stmt(states, stmt->as.select_case.body, can_escape);
            return;
        case AST_DEFER_STMT:
            xa_os_resource_lint_scan_defer_expr(states, stmt->as.defer_stmt.expr, can_escape);
            return;
        case AST_SCOPE_BLOCK:
            xa_os_resource_lint_scan_stmt(states, stmt->as.scope_block.body, can_escape);
            return;
        case AST_EXPORT_STMT:
            xa_os_resource_lint_scan_stmt(states, stmt->as.export_stmt.declaration, can_escape);
            return;
        default:
            xa_os_resource_lint_scan_expr(states, stmt, false, can_escape);
            return;
    }
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_summarize_function_node(XaInferContext *ctx, AstNode *fn_node,
                                            const char *summary_name, uint32_t summary_symbol_id,
                                            XaOsResourceLintFnSummary *visible_summaries) {
    FunctionDeclNode *fn = xa_lifecycle_lint_function_node(fn_node);
    if (!ctx || !fn_node || !fn || !summary_name || !fn->body ||
        xa_lifecycle_lint_body_has_non_tail_exit(fn->body))
        return NULL;

    XaOsResourceLintFnSummary *summary = xr_calloc(1, sizeof(XaOsResourceLintFnSummary));
    if (!summary)
        return NULL;
    summary->name = summary_name;
    summary->symbol_id = summary_symbol_id;
    summary->return_param_index = -1;
    summary->param_count = fn->param_count;
    summary->params = fn->param_count > 0
                          ? xr_calloc((size_t) fn->param_count, sizeof(XaOsResourceParamSummary))
                          : NULL;
    XaOsResourceLintState **by_index =
        fn->param_count > 0 ? xr_calloc((size_t) fn->param_count, sizeof(XaOsResourceLintState *))
                            : NULL;
    if ((fn->param_count > 0 && !summary->params) || (fn->param_count > 0 && !by_index)) {
        xr_free(by_index);
        xa_os_resource_lint_free_fn_summaries(summary);
        return NULL;
    }

    XaOsResourceLintState *states = NULL;
    XaOsResourceLintState **tail = &states;
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
        if (!param || !xa_lifecycle_lint_function_param_os_resource_kind(ctx, fn, i, &kind))
            continue;
        XaOsResourceLintState *state = xr_calloc(1, sizeof(XaOsResourceLintState));
        if (!state)
            continue;
        state->ctx = ctx;
        state->kind = kind;
        if (param->symbol_id != 0)
            xa_os_resource_lint_add_alias_id(state, param->symbol_id);
        xa_os_resource_lint_add_alias_name(state, param->name);
        XaSymbol *param_sym = xa_lifecycle_lint_function_param_symbol(ctx, fn_node, param);
        if (param_sym)
            xa_os_resource_lint_add_alias(state, param_sym);
        by_index[i] = state;
        *tail = state;
        tail = &state->next;
    }

    AstNode *return_expr =
        xa_lifecycle_lint_returned_handle_expr(xa_lifecycle_lint_tail_return_expr(fn->body));
    summary->returns_new_resource = xa_os_resource_lint_expr_returns_new_resource(
        ctx, visible_summaries, return_expr, &summary->return_kind);

    XaOsResourceLintState *returned_state = NULL;
    if (states) {
        for (XaOsResourceLintState *s = states; s; s = s->next)
            s->fn_summaries = visible_summaries;
        xa_os_resource_lint_scan_stmt(states, fn->body, true);
        returned_state = xa_os_resource_lint_find_alias_source(states, return_expr);
    }
    bool any_finalized = false;
    bool returns_param = false;
    for (int i = 0; i < fn->param_count; i++) {
        XaOsResourceLintState *state = by_index[i];
        if (!state)
            continue;
        XaOsResourceParamSummary *param = &summary->params[i];
        param->is_resource = true;
        param->kind = state->kind;
        if (state == returned_state) {
            summary->return_param_index = i;
            returns_param = true;
        }
        param->finalized = state->finalized && !state->transferred;
        param->pipe_read_closed = state->pipe_read_closed && !state->transferred;
        param->pipe_write_closed = state->pipe_write_closed && !state->transferred;
        any_finalized = any_finalized || param->finalized ||
                        (param->kind == XA_OS_RESOURCE_PIPE && param->pipe_read_closed &&
                         param->pipe_write_closed);
    }

    xa_os_resource_lint_free_states(states);
    xr_free(by_index);
    if (!any_finalized && !returns_param && !summary->returns_new_resource) {
        xa_os_resource_lint_free_fn_summaries(summary);
        return NULL;
    }
    return summary;
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_summarize_function(XaInferContext *ctx, AstNode *stmt,
                                       XaOsResourceLintFnSummary *visible_summaries) {
    if (!stmt || stmt->type != AST_FUNCTION_DECL)
        return NULL;
    FunctionDeclNode *fn = &stmt->as.function_decl;
    return xa_os_resource_lint_summarize_function_node(ctx, stmt, fn->name, fn->symbol_id,
                                                       visible_summaries);
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_clone_fn_summary_as(XaOsResourceLintFnSummary *source, const char *name,
                                        uint32_t symbol_id) {
    if (!source || !name)
        return NULL;
    XaOsResourceLintFnSummary *summary = xr_calloc(1, sizeof(XaOsResourceLintFnSummary));
    if (!summary)
        return NULL;
    summary->name = name;
    summary->symbol_id = symbol_id;
    summary->return_param_index = source->return_param_index;
    summary->returns_new_resource = source->returns_new_resource;
    summary->return_kind = source->return_kind;
    summary->param_count = source->param_count;
    summary->params = source->param_count > 0 ? xr_calloc((size_t) source->param_count,
                                                          sizeof(XaOsResourceParamSummary))
                                              : NULL;
    if (source->param_count > 0 && !summary->params) {
        xa_os_resource_lint_free_fn_summaries(summary);
        return NULL;
    }
    if (source->param_count > 0 && source->params)
        memcpy(summary->params, source->params,
               sizeof(XaOsResourceParamSummary) * (size_t) source->param_count);
    return summary;
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_summarize_const_function_value(XaInferContext *ctx, AstNode *stmt,
                                                   XaOsResourceLintFnSummary *visible_summaries) {
    if (!stmt || stmt->type != AST_CONST_DECL)
        return NULL;
    VarDeclNode *var = &stmt->as.var_decl;
    AstNode *initializer = xa_thread_lint_unwrap_expr(var->initializer);
    if (!var->name || var->storage_mode != XR_STORAGE_NORMAL || !initializer)
        return NULL;
    if (initializer->type == AST_FUNCTION_EXPR)
        return xa_os_resource_lint_summarize_function_node(ctx, initializer, var->name,
                                                           var->symbol_id, visible_summaries);
    if (initializer->type != AST_VARIABLE)
        return NULL;
    XaOsResourceLintFnSummary *source =
        xa_os_resource_lint_find_fn_summary(ctx, visible_summaries, initializer);
    return xa_os_resource_lint_clone_fn_summary_as(source, var->name, var->symbol_id);
}

static bool xa_os_resource_lint_fn_summary_exists(XaOsResourceLintFnSummary *summaries,
                                                  uint32_t symbol_id, const char *name) {
    for (XaOsResourceLintFnSummary *s = summaries; s; s = s->next) {
        if (symbol_id != 0 && s->symbol_id == symbol_id)
            return true;
        if (symbol_id == 0 && s->symbol_id == 0 && name && s->name && strcmp(s->name, name) == 0)
            return true;
    }
    return false;
}

static bool xa_os_resource_lint_collect_statement_fn_summaries(XaInferContext *ctx,
                                                               AstNode **statements, int count,
                                                               XaOsResourceLintFnSummary **head,
                                                               XaOsResourceLintFnSummary ***tail) {
    if (!ctx || !statements || count <= 0 || !head || !tail || !*tail)
        return false;
    bool added = false;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        XaOsResourceLintFnSummary *summary = NULL;
        if (stmt->type == AST_FUNCTION_DECL) {
            FunctionDeclNode *fn = &stmt->as.function_decl;
            if (!xa_os_resource_lint_fn_summary_exists(*head, fn->symbol_id, fn->name))
                summary = xa_os_resource_lint_summarize_function(ctx, stmt, *head);
        } else if (stmt->type == AST_CONST_DECL) {
            VarDeclNode *var = &stmt->as.var_decl;
            if (!xa_os_resource_lint_fn_summary_exists(*head, var->symbol_id, var->name))
                summary = xa_os_resource_lint_summarize_const_function_value(ctx, stmt, *head);
        }
        if (!summary)
            continue;
        **tail = summary;
        *tail = &summary->next;
        added = true;
    }
    return added;
}

static bool xa_os_resource_lint_collect_scope_fn_summaries(XaInferContext *ctx, XaScope *scope,
                                                           XaOsResourceLintFnSummary **head,
                                                           XaOsResourceLintFnSummary ***tail) {
    if (!ctx || !scope || !head || !tail || !*tail)
        return false;
    bool added = false;
    AstNode *node = scope->kind == XA_SCOPE_FUNCTION ? (AstNode *) scope->ast_node : NULL;
    if (node && node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        if (!xa_os_resource_lint_fn_summary_exists(*head, fn->symbol_id, fn->name)) {
            XaOsResourceLintFnSummary *summary =
                xa_os_resource_lint_summarize_function(ctx, node, *head);
            if (summary) {
                **tail = summary;
                *tail = &summary->next;
                added = true;
            }
        }
    }
    int symbol_count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &symbol_count);
    for (int i = 0; i < symbol_count; i++) {
        XaSymbol *sym = symbols ? symbols[i] : NULL;
        if (!sym || sym->kind != XA_SYM_VARIABLE || !sym->is_const || sym->is_shared ||
            sym->is_imported || !sym->name)
            continue;
        if (xa_os_resource_lint_fn_summary_exists(*head, sym->id, sym->name))
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
        AstNode *initializer = links ? xa_thread_lint_unwrap_expr(links->const_initializer) : NULL;
        if (!initializer ||
            (initializer->type != AST_FUNCTION_EXPR && initializer->type != AST_VARIABLE))
            continue;
        XaOsResourceLintFnSummary *summary = NULL;
        if (initializer->type == AST_FUNCTION_EXPR) {
            summary = xa_os_resource_lint_summarize_function_node(ctx, initializer, sym->name,
                                                                  sym->id, *head);
        } else {
            XaOsResourceLintFnSummary *source =
                xa_os_resource_lint_find_fn_summary(ctx, *head, initializer);
            summary = xa_os_resource_lint_clone_fn_summary_as(source, sym->name, sym->id);
        }
        if (summary) {
            **tail = summary;
            *tail = &summary->next;
            added = true;
        }
    }
    xr_free(symbols);
    for (int i = 0; i < scope->child_count; i++)
        added =
            xa_os_resource_lint_collect_scope_fn_summaries(ctx, scope->children[i], head, tail) ||
            added;
    return added;
}

static XaOsResourceLintFnSummary *
xa_os_resource_lint_collect_visible_fn_summaries(XaInferContext *ctx, AstNode **statements,
                                                 int count) {
    XaOsResourceLintFnSummary *summaries = NULL;
    XaOsResourceLintFnSummary **tail = &summaries;
    XaScope *root = xa_lifecycle_lint_current_root_scope(ctx);
    bool added = false;
    do {
        added = xa_os_resource_lint_collect_statement_fn_summaries(ctx, statements, count,
                                                                   &summaries, &tail);
        if (root)
            added = xa_os_resource_lint_collect_scope_fn_summaries(ctx, root, &summaries, &tail) ||
                    added;
        if (ctx && ctx->analyzer && ctx->analyzer->global_scope &&
            ctx->analyzer->global_scope != root) {
            added = xa_os_resource_lint_collect_scope_fn_summaries(ctx, ctx->analyzer->global_scope,
                                                                   &summaries, &tail) ||
                    added;
        }
    } while (added);
    return summaries;
}

static void xa_os_resource_lint_attach_fn_summaries(XaOsResourceLintState *states,
                                                    XaOsResourceLintFnSummary *summaries) {
    for (XaOsResourceLintState *s = states; s; s = s->next)
        s->fn_summaries = summaries;
}

static XaOsResourceLintState **xa_os_resource_lint_append_state(XaInferContext *ctx,
                                                                AstNode *decl_stmt, XaSymbol *sym,
                                                                XaOsResourceKind kind,
                                                                XaOsResourceLintState **tail);

static XaOsResourceLintState **xa_os_resource_lint_collect_destructure_states(
    XaInferContext *ctx, AstNode *decl_stmt, XrDestructurePattern *pattern, AstNode *initializer,
    XaOsResourceLintFnSummary *fn_summaries, XaOsResourceLintState **tail);

static XaOsResourceLintState *
xa_os_resource_lint_collect_states(XaInferContext *ctx, AstNode **statements, int count,
                                   XaOsResourceLintFnSummary *fn_summaries) {
    if (!ctx || !statements || count <= 0)
        return NULL;
    XaOsResourceLintState *states = NULL;
    XaOsResourceLintState **tail = &states;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_VAR_DECL || stmt->type == AST_CONST_DECL) {
            VarDeclNode *var = &stmt->as.var_decl;
            XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
            if (!var->name || !var->initializer ||
                (!xa_expr_is_sys_os_resource_open_call(ctx, var->initializer, &kind) &&
                 !xa_os_resource_lint_expr_returns_new_resource(ctx, fn_summaries, var->initializer,
                                                                &kind)))
                continue;
            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
            tail = xa_os_resource_lint_append_state(ctx, stmt, sym, kind, tail);
            continue;
        }
        if (stmt->type == AST_DESTRUCTURE_DECL) {
            tail = xa_os_resource_lint_collect_destructure_states(
                ctx, stmt, stmt->as.destructure_decl.pattern, stmt->as.destructure_decl.initializer,
                fn_summaries, tail);
            continue;
        }
    }
    return states;
}

static XaOsResourceLintState **xa_os_resource_lint_append_state(XaInferContext *ctx,
                                                                AstNode *decl_stmt, XaSymbol *sym,
                                                                XaOsResourceKind kind,
                                                                XaOsResourceLintState **tail) {
    if (!ctx || !tail || !sym)
        return tail;
    if (!xa_symbol_is_local_os_resource_handle(sym, ctx->analyzer->current_scope, kind))
        return tail;
    XaOsResourceLintState *state = xr_calloc(1, sizeof(XaOsResourceLintState));
    if (!state)
        return tail;
    state->ctx = ctx;
    state->kind = kind;
    state->root = sym;
    state->decl_stmt = decl_stmt;
    xa_os_resource_lint_add_alias(state, sym);
    *tail = state;
    return &state->next;
}

static XaOsResourceLintState **xa_os_resource_lint_collect_destructure_states(
    XaInferContext *ctx, AstNode *decl_stmt, XrDestructurePattern *pattern, AstNode *initializer,
    XaOsResourceLintFnSummary *fn_summaries, XaOsResourceLintState **tail) {
    if (!ctx || !ctx->analyzer || !pattern || !initializer || !tail)
        return tail;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER: {
            XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
            if (!xa_os_resource_lint_expr_returns_new_resource(ctx, fn_summaries, initializer,
                                                               &kind))
                return tail;
            XaSymbol *sym = NULL;
            if (pattern->as.identifier.symbol_id != 0)
                sym = xa_scope_lookup_by_id(ctx->analyzer->current_scope,
                                            pattern->as.identifier.symbol_id);
            if (!sym && pattern->as.identifier.name)
                sym = xa_scope_lookup(ctx->analyzer->current_scope, pattern->as.identifier.name);
            return xa_os_resource_lint_append_state(ctx, decl_stmt, sym, kind, tail);
        }
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                tail = xa_os_resource_lint_collect_destructure_states(
                    ctx, decl_stmt, pattern->as.array.elements[i],
                    xa_lifecycle_lint_destructure_source_at(initializer, i), fn_summaries, tail);
            }
            return tail;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                tail = xa_os_resource_lint_collect_destructure_states(
                    ctx, decl_stmt, pattern->as.object.patterns[i],
                    xa_lifecycle_lint_object_source_for_field(initializer,
                                                              pattern->as.object.field_names[i]),
                    fn_summaries, tail);
            }
            return tail;
        default:
            return tail;
    }
}

static void xa_warn_unused_os_resource_decl(XaInferContext *ctx, AstNode *stmt,
                                            XaOsResourceLintFnSummary *fn_summaries) {
    if (!ctx || !ctx->analyzer || !stmt ||
        (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
        return;

    VarDeclNode *var = &stmt->as.var_decl;
    XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
    if (!var->name || !var->initializer ||
        (!xa_expr_is_sys_os_resource_open_call(ctx, var->initializer, &kind) &&
         !xa_os_resource_lint_expr_returns_new_resource(ctx, fn_summaries, var->initializer,
                                                        &kind)))
        return;

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!xa_symbol_is_local_os_resource_handle(sym, ctx->analyzer->current_scope, kind))
        return;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || links->ref_count != 0)
        return;

    char msg[224];
    snprintf(msg, sizeof(msg), "%s handle '%s' from %s is never used; call %s() explicitly",
             xa_os_resource_type_name(kind), var->name, xa_os_resource_open_name(kind),
             xa_os_resource_close_method(kind));
    XrLocation loc = {.file = ctx->file_path, .line = stmt->line, .column = stmt->column};
    if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_warn_discarded_os_resource_factory_expr(XaInferContext *ctx, AstNode *stmt,
                                                       XaOsResourceLintFnSummary *fn_summaries) {
    if (!ctx || !ctx->analyzer || !stmt || stmt->type != AST_EXPR_STMT)
        return;
    AstNode *expr = stmt->as.expr_stmt;
    XaOsResourceKind kind = XA_OS_RESOURCE_PROCESS;
    if (!xa_os_resource_lint_expr_returns_new_resource(ctx, fn_summaries, expr, &kind))
        return;
    char msg[192];
    snprintf(msg, sizeof(msg), "%s returns a %s handle; call %s() explicitly",
             xa_os_resource_open_name(kind), xa_os_resource_type_name(kind),
             xa_os_resource_close_method(kind));
    XrLocation loc = {.file = ctx->file_path,
                      .line = expr ? expr->line : stmt->line,
                      .column = expr ? expr->column : stmt->column};
    if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_warn_os_resource_lifecycle_in_sequence(XaInferContext *ctx, AstNode **statements,
                                                      int count, int error_count_before) {
    if (!ctx || !statements || count <= 0)
        return;
    if (xa_analyzer_error_diagnostic_count(ctx->analyzer) != error_count_before)
        return;

    XaOsResourceLintFnSummary *fn_summaries =
        xa_os_resource_lint_collect_visible_fn_summaries(ctx, statements, count);

    for (int i = 0; i < count; i++) {
        xa_warn_unused_os_resource_decl(ctx, statements[i], fn_summaries);
        xa_warn_discarded_os_resource_factory_expr(ctx, statements[i], fn_summaries);
    }

    XaOsResourceLintState *states =
        xa_os_resource_lint_collect_states(ctx, statements, count, fn_summaries);
    if (!states) {
        xa_os_resource_lint_free_fn_summaries(fn_summaries);
        return;
    }
    xa_os_resource_lint_attach_fn_summaries(states, fn_summaries);
    for (int i = 0; i < count; i++)
        xa_os_resource_lint_scan_stmt(states, statements[i], true);
    for (XaOsResourceLintState *state = states; state; state = state->next) {
        if (!state->root || !state->decl_stmt || state->finalized || state->transferred)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, state->root);
        if ((!links || links->ref_count == 0) && state->decl_stmt->type != AST_DESTRUCTURE_DECL)
            continue;
        char msg[224];
        snprintf(msg, sizeof(msg), "%s handle '%s' from %s is not %s before leaving scope",
                 xa_os_resource_type_name(state->kind), state->root->name ? state->root->name : "?",
                 xa_os_resource_open_name(state->kind),
                 xa_os_resource_close_past_tense(state->kind));
        XrLocation loc = {.file = ctx->file_path,
                          .line = state->decl_stmt->line,
                          .column = state->decl_stmt->column};
        if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg,
                                       &loc);
    }
    xa_os_resource_lint_free_states(states);
    xa_os_resource_lint_free_fn_summaries(fn_summaries);
}

static bool xa_freestanding_top_const_allowed(XaInferContext *ctx, VarDeclNode *var) {
    if (!ctx || !ctx->analyzer || !var || !var->initializer)
        return false;
    XrCtValue value = {0};
    const char *err = NULL;
    (void) err;
    if (!xa_consteval_expr(ctx->analyzer, var->initializer, &value, &err))
        return false;
    return xa_freestanding_top_const_ct_value_allowed(&value);
}

static bool xa_freestanding_shared_static_initializer_allowed(XaInferContext *ctx, VarDeclNode *var,
                                                              bool allow_aggregate) {
    if (!ctx || !ctx->analyzer || !var || !var->initializer)
        return false;
    XrCtValue value = {0};
    const char *err = NULL;
    (void) err;
    if (!xa_consteval_expr(ctx->analyzer, var->initializer, &value, &err))
        return false;
    switch (value.kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_STRING:
        case XR_CT_NULL:
            return true;
        case XR_CT_STRUCT_VALUE:
            return allow_aggregate &&
                   xa_freestanding_top_const_aggregate_value_allowed(&value, false);
        default:
            return false;
    }
}

static bool xa_freestanding_top_var_static_initializer_allowed(XaInferContext *ctx,
                                                               VarDeclNode *var,
                                                               XrType *declared_type) {
    if (!var)
        return false;
    if (var->initializer)
        return xa_freestanding_shared_static_initializer_allowed(ctx, var, true);
    if (!declared_type || XR_TYPE_IS_UNKNOWN(declared_type) ||
        !xa_type_is_default_initializable(ctx, declared_type))
        return false;
    XrType *base = declared_type->is_nullable
                       ? xr_type_non_nullable(ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL,
                                              declared_type)
                       : declared_type;
    if (!base)
        return false;
    switch (base->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_STRING:
        case XR_KIND_NULL:
            return true;
        default:
            return false;
    }
}

XR_FUNC void xa_loop_scope_push(XaInferContext *ctx, XaLoopScope *scope, const char *label,
                                AstNode *node) {
    if (!ctx || !scope)
        return;
    scope->label = label;
    scope->line = node ? node->line : 0;
    scope->prev = ctx->loop_scope;

    if (label) {
        for (XaLoopScope *it = ctx->loop_scope; it; it = it->prev) {
            if (it->label && strcmp(it->label, label) == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node ? node->line : 0, .column = 1};
                char msg[160];
                snprintf(msg, sizeof(msg), "duplicate loop label '%s' in an active loop", label);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg,
                                           &loc);
                break;
            }
        }
    }

    ctx->loop_scope = scope;
    ctx->loop_depth++;
}

XR_FUNC void xa_loop_scope_pop(XaInferContext *ctx, XaLoopScope *scope) {
    if (!ctx || !scope)
        return;
    if (ctx->loop_scope == scope)
        ctx->loop_scope = scope->prev;
    else
        ctx->loop_scope = scope->prev;
    if (ctx->loop_depth > 0)
        ctx->loop_depth--;
}

XR_FUNC void xa_validate_loop_control(XaInferContext *ctx, AstNode *node, const char *label,
                                      bool is_continue) {
    if (!ctx || !node)
        return;

    const char *kind = is_continue ? "continue" : "break";
    int code = is_continue ? XR_ERR_CMP_INVALID_CONTINUE : XR_ERR_CMP_INVALID_BREAK;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};

    if (label) {
        for (XaLoopScope *it = ctx->loop_scope; it; it = it->prev) {
            if (it->label && strcmp(it->label, label) == 0)
                return;
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown loop label '%s' for '%s'", label, kind);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
        return;
    }

    if (ctx->loop_depth <= 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "'%s' outside of a loop", kind);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
    }
}

static bool xa_function_assignment_mismatch(XrType *target_type, XrType *value_type) {
    if (!target_type || !value_type)
        return true;
    if (XR_TYPE_IS_NULL(value_type))
        return !target_type->is_nullable;
    if (!XR_TYPE_IS_FUNCTION(value_type))
        return true;
    if (target_type->is_nullable || value_type->is_nullable)
        return !xa_typecheck_assignable(target_type, value_type);
    return !xr_type_equals(target_type, value_type);
}

XR_FUNC void xa_assign_check_type(XaInferContext *ctx, AstNode *node, XrType *target_type,
                                  XrType *value_type, const char *target_name,
                                  const char *target_kind) {
    if (!ctx || !node || !target_type || !value_type)
        return;
    if (XR_TYPE_IS_UNKNOWN(target_type) || XR_TYPE_IS_UNKNOWN(value_type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    bool null_err =
        xa_check_null_safety(ctx->analyzer, target_type, value_type, "Assignment", &loc);
    bool type_mismatch = false;
    if (XR_TYPE_IS_FUNCTION(target_type)) {
        type_mismatch = xa_function_assignment_mismatch(target_type, value_type);
    } else {
        type_mismatch = !xa_typecheck_assignable(target_type, value_type);
    }
    if (null_err || !type_mismatch)
        return;
    if (!XR_TYPE_IS_FUNCTION(target_type) && xr_is_json_coercion(target_type, value_type))
        return;

    char msg[256];
    if (target_name && target_kind) {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to %s '%s' (type '%s')",
                 xr_type_to_string(value_type), target_kind, target_name,
                 xr_type_to_string(target_type));
    } else if (target_name) {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to '%s' (type '%s')",
                 xr_type_to_string(value_type), target_name, xr_type_to_string(target_type));
    } else {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to type '%s'",
                 xr_type_to_string(value_type), xr_type_to_string(target_type));
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

XR_FUNC bool xa_type_needs_borrow_escape_guard(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (type->is_value_type)
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        int n = xr_type_union_count(type);
        for (int i = 0; i < n; i++) {
            if (xa_type_needs_borrow_escape_guard(xr_type_union_member(type, i)))
                return true;
        }
        return false;
    }
    switch (type->kind) {
        case XR_KIND_SPAN:
        case XR_KIND_VIEW:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_JSON:
        case XR_KIND_RECORD:
        case XR_KIND_INSTANCE:
        case XR_KIND_FUNCTION:
        case XR_KIND_TUPLE:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_TYPE_PARAM:
            return true;
        default:
            return false;
    }
}

XR_FUNC XaSymbol *xa_borrowed_param_root_symbol(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE: {
                const char *name = expr->as.variable.name;
                if (!name)
                    return NULL;
                XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
                if (sym && sym->kind == XA_SYM_PARAMETER &&
                    (sym->passing_mode == XR_PARAM_IN || sym->passing_mode == XR_PARAM_REF))
                    return sym;
                if (sym && sym->borrowed_root_symbol_id != 0) {
                    XaSymbol *root = xa_scope_lookup_by_id(ctx->analyzer->current_scope,
                                                           sym->borrowed_root_symbol_id);
                    if (root && root->kind == XA_SYM_PARAMETER &&
                        (root->passing_mode == XR_PARAM_IN || root->passing_mode == XR_PARAM_REF))
                        return root;
                }
                return NULL;
            }
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
            case AST_OPTIONAL_CHAIN:
                expr = expr->as.optional_chain.object;
                break;
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
                return NULL;
        }
    }
    return NULL;
}

XR_FUNC bool xa_type_contains_span_view(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (XR_TYPE_IS_SPAN(type))
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        int n = xr_type_union_count(type);
        for (int i = 0; i < n; i++) {
            if (xa_type_contains_span_view(xr_type_union_member(type, i)))
                return true;
        }
        return false;
    }
    if (XR_TYPE_IS_TUPLE(type)) {
        for (int i = 0; i < type->tuple.element_count; i++) {
            if (xa_type_contains_span_view(type->tuple.element_types[i]))
                return true;
        }
        return false;
    }
    if (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_VIEW(type) || type->kind == XR_KIND_SET ||
        type->kind == XR_KIND_CHANNEL) {
        return xa_type_contains_span_view(type->container.element_type);
    }
    if (XR_TYPE_IS_MAP(type)) {
        return xa_type_contains_span_view(type->map.key_type) ||
               xa_type_contains_span_view(type->map.value_type);
    }
    if (type->kind == XR_KIND_FIXED_ARRAY) {
        return xa_type_contains_span_view(type->fixed_array.element_type);
    }
    if (XR_TYPE_HAS_OBJECT_SHAPE(type) && type->object.field_types) {
        for (int i = 0; i < type->object.field_count; i++) {
            if (xa_type_contains_span_view(type->object.field_types[i]))
                return true;
        }
    }
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE ||
         type->kind == XR_KIND_INTERFACE) &&
        type->instance.type_args) {
        for (int i = 0; i < type->instance.type_arg_count; i++) {
            if (xa_type_contains_span_view(type->instance.type_args[i]))
                return true;
        }
    }
    return false;
}

static bool xa_call_expr_is_borrowed_view(AstNode *expr) {
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const char *name = call->callee->as.member_access.name;
    return name && (strcmp(name, "bytes") == 0 || strcmp(name, "asBytes") == 0 ||
                    strcmp(name, "asMutBytes") == 0 || strcmp(name, "reinterpret") == 0);
}

static bool xa_call_expr_is_mem_view_syntax(AstNode *expr) {
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS || call->arg_count != 1 ||
        !call->arguments || !call->arguments[0])
        return false;
    MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "view") == 0 && member->object &&
           member->object->type == AST_VARIABLE && member->object->as.variable.name &&
           strcmp(member->object->as.variable.name, "mem") == 0;
}

static bool xa_call_expr_is_mem_view(XaInferContext *ctx, AstNode *expr) {
    if (xa_call_expr_is_mem_view_syntax(expr))
        return true;
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS || call->arg_count != 1 ||
        !call->arguments || !call->arguments[0])
        return false;
    MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name || strcmp(member->name, "view") != 0 || !member->object ||
        member->object->type != AST_VARIABLE || !member->object->as.variable.name)
        return false;
    XaSymbol *symbol = xa_lookup_visible_symbol(ctx, member->object->as.variable.name);
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(ctx->analyzer, symbol) : NULL;
    return links && links->module_name && strcmp(links->module_name, "mem") == 0;
}

static AstNode *xa_unsafe_expr_result(AstNode *expr) {
    if (!expr || expr->type != AST_UNSAFE_EXPR)
        return NULL;
    AstNode *body = expr->as.unsafe_expr.operand;
    if (!body || body->type != AST_BLOCK)
        return body;
    BlockNode *block = &body->as.block;
    if (block->count <= 0 || !block->statements[block->count - 1] ||
        block->statements[block->count - 1]->type != AST_EXPR_STMT)
        return NULL;
    return block->statements[block->count - 1]->as.expr_stmt;
}

static bool xa_call_expr_preserves_owner_borrow(XaInferContext *ctx, AstNode *expr,
                                                bool *out_pointer_borrow) {
    if (out_pointer_borrow)
        *out_pointer_borrow = false;
    if (xa_call_expr_is_borrowed_view(expr))
        return true;
    if (xa_call_expr_is_mem_view(ctx, expr)) {
        if (out_pointer_borrow)
            *out_pointer_borrow = true;
        return true;
    }
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name || !member->object)
        return false;
    XrType *receiver_type = xa_analyzer_get_node_type(ctx->analyzer, member->object);
    bool preserves = false;
    if ((strcmp(member->name, "ptr") == 0 || strcmp(member->name, "mutPtr") == 0) &&
        receiver_type &&
        (XR_TYPE_IS_ARRAY(receiver_type) || XR_TYPE_IS_SPAN(receiver_type) ||
         receiver_type->kind == XR_KIND_FIXED_ARRAY)) {
        preserves = true;
    } else if (strcmp(member->name, "borrowPtr") == 0 && receiver_type &&
               xr_type_is_named_class(receiver_type, "Buffer")) {
        preserves = true;
    } else if (strcmp(member->name, "offset") == 0 && receiver_type &&
               XR_TYPE_IS_POINTER(receiver_type)) {
        preserves = true;
    }
    if (preserves && out_pointer_borrow)
        *out_pointer_borrow = true;
    return preserves;
}

XR_FUNC bool xa_expr_has_stable_borrow_owner(AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
            case AST_THIS_EXPR:
                return true;
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
            case AST_OPTIONAL_CHAIN:
                expr = expr->as.optional_chain.object;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_UNSAFE_EXPR:
                expr = xa_unsafe_expr_result(expr);
                break;
            case AST_CALL_EXPR:
                if (xa_call_expr_is_mem_view_syntax(expr)) {
                    expr = expr->as.call_expr.arguments[0];
                    break;
                }
                if (!xa_call_expr_is_borrowed_view(expr))
                    return false;
                expr = expr->as.call_expr.callee->as.member_access.object;
                break;
            default:
                return false;
        }
    }
    return false;
}

XR_FUNC bool xa_type_can_own_span_view(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_STRING(type))
        return true;
    return xr_type_is_named_class(type, "Buffer");
}

XR_FUNC XaSymbol *xa_root_variable_symbol_for_expr(XaInferContext *ctx, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return expr->as.variable.name
                           ? xa_lookup_visible_symbol(ctx, expr->as.variable.name)
                           : NULL;
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            case AST_UNSAFE_EXPR:
                expr = xa_unsafe_expr_result(expr);
                break;
            default:
                return NULL;
        }
    }
    return NULL;
}

XR_FUNC bool xa_symbol_has_shared_provenance(const XaSymbol *sym) {
    return sym && (sym->is_shared || sym->is_shared_provenance);
}

static XaSymbol *xa_shared_provenance_root_symbol_for_expr(XaInferContext *ctx, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return expr->as.variable.name
                           ? xa_lookup_visible_symbol(ctx, expr->as.variable.name)
                           : NULL;
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
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
                return NULL;
        }
    }
    return NULL;
}

XR_FUNC bool xa_expr_yields_shared_provenance(XaInferContext *ctx, AstNode *expr, XrType *type) {
    if (!ctx || !expr || !xa_type_needs_borrow_escape_guard(type))
        return false;
    XaSymbol *root = xa_shared_provenance_root_symbol_for_expr(ctx, expr);
    return xa_symbol_has_shared_provenance(root);
}

static XaActiveSpanBorrow *xa_active_span_borrow_for_view(XaInferContext *ctx, XaSymbol *view_sym) {
    if (!ctx || !view_sym)
        return NULL;
    for (XaActiveSpanBorrow *b = ctx->active_span_borrows; b; b = b->next) {
        if (b->view_symbol == view_sym)
            return b;
    }
    return NULL;
}

static AstNode *xa_unwrap_owner_borrow_expr(AstNode *expr) {
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
            case AST_UNSAFE_EXPR:
                expr = xa_unsafe_expr_result(expr);
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static bool xa_pointer_expr_has_owner_borrow(XaInferContext *ctx, AstNode *expr) {
    expr = xa_unwrap_owner_borrow_expr(expr);
    if (!ctx || !expr)
        return false;
    if (expr->type == AST_VARIABLE) {
        XaSymbol *sym =
            expr->as.variable.name ? xa_lookup_visible_symbol(ctx, expr->as.variable.name) : NULL;
        XaActiveSpanBorrow *borrow = xa_active_span_borrow_for_view(ctx, sym);
        return borrow && borrow->is_pointer_borrow;
    }
    if (expr->type == AST_MEMBER_ACCESS) {
        XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
        return type && XR_TYPE_IS_POINTER(type) && type->ptr_is_c_view &&
               xa_pointer_expr_has_owner_borrow(ctx, expr->as.member_access.object);
    }
    if (expr->type != AST_CALL_EXPR || !expr->as.call_expr.callee ||
        expr->as.call_expr.callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *member = &expr->as.call_expr.callee->as.member_access;
    if (xa_call_expr_is_mem_view(ctx, expr))
        return xa_pointer_expr_has_owner_borrow(ctx, expr->as.call_expr.arguments[0]);
    return member->name && strcmp(member->name, "offset") == 0 &&
           xa_pointer_expr_has_owner_borrow(ctx, member->object);
}

static bool xa_path_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src)
        return false;
    int n = snprintf(dst, dst_size, "%s", src);
    return n >= 0 && (size_t) n < dst_size;
}

static bool xa_path_append(char *dst, size_t dst_size, const char *suffix) {
    if (!dst || dst_size == 0 || !suffix)
        return false;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return false;
    int n = snprintf(dst + len, dst_size - len, "%s", suffix);
    return n >= 0 && (size_t) n < dst_size - len;
}

static bool xa_index_path_segment(AstNode *index, char *buf, size_t buf_size) {
    if (!index || !buf || buf_size == 0)
        return false;
    while (index && (index->type == AST_GROUPING || index->type == AST_FORCE_UNWRAP))
        index = index->type == AST_GROUPING ? index->as.grouping : index->as.unary.operand;
    if (!index || index->type != AST_LITERAL_INT)
        return false;
    int n = snprintf(buf, buf_size, "[%lld]", (long long) index->as.literal.raw_value.int_val);
    return n >= 0 && (size_t) n < buf_size;
}

static bool xa_path_is_same_or_nested(const char *path, const char *prefix) {
    if (!path || !prefix)
        return true;
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '.' || path[len] == '[';
}

static bool xa_owner_paths_may_overlap(const char *borrow_path, const char *mutation_path) {
    if (!borrow_path || !mutation_path)
        return true;
    return xa_path_is_same_or_nested(borrow_path, mutation_path) ||
           xa_path_is_same_or_nested(mutation_path, borrow_path);
}

static XaSymbol *xa_root_path_for_expr(XaInferContext *ctx, AstNode *expr, char *path_buf,
                                       size_t path_buf_size, bool *out_precise,
                                       bool follow_active_view) {
    if (!ctx || !expr || !path_buf || path_buf_size == 0)
        return NULL;
    if (out_precise)
        *out_precise = true;

    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.unary.operand;
    if (!expr)
        return NULL;

    switch (expr->type) {
        case AST_VARIABLE: {
            XaSymbol *sym = expr->as.variable.name
                                ? xa_lookup_visible_symbol(ctx, expr->as.variable.name)
                                : NULL;
            XaActiveSpanBorrow *active =
                follow_active_view ? xa_active_span_borrow_for_view(ctx, sym) : NULL;
            if (active) {
                if (active->owner_path) {
                    xa_path_copy(path_buf, path_buf_size, active->owner_path);
                } else if (active->owner_symbol && active->owner_symbol->name) {
                    xa_path_copy(path_buf, path_buf_size, active->owner_symbol->name);
                    if (out_precise)
                        *out_precise = false;
                }
                return active->owner_symbol;
            }
            if (!sym || !sym->name || !xa_path_copy(path_buf, path_buf_size, sym->name))
                return NULL;
            return sym;
        }
        case AST_MEMBER_ACCESS: {
            MemberAccessNode *ma = &expr->as.member_access;
            bool precise = true;
            XaSymbol *root = xa_root_path_for_expr(ctx, ma->object, path_buf, path_buf_size,
                                                   &precise, follow_active_view);
            if (!root)
                return NULL;
            if (precise && ma->name) {
                if (!xa_path_append(path_buf, path_buf_size, ".") ||
                    !xa_path_append(path_buf, path_buf_size, ma->name))
                    precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_INDEX_GET: {
            IndexGetNode *ig = &expr->as.index_get;
            bool precise = true;
            XaSymbol *root = xa_root_path_for_expr(ctx, ig->array, path_buf, path_buf_size,
                                                   &precise, follow_active_view);
            if (!root)
                return NULL;
            char segment[64];
            if (precise && xa_index_path_segment(ig->index, segment, sizeof(segment))) {
                if (!xa_path_append(path_buf, path_buf_size, segment))
                    precise = false;
            } else {
                xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
                precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_SLICE_EXPR:
            return xa_root_path_for_expr(ctx, expr->as.slice_expr.source, path_buf, path_buf_size,
                                         out_precise, follow_active_view);
        case AST_OPTIONAL_CHAIN:
            return xa_root_path_for_expr(ctx, expr->as.optional_chain.object, path_buf,
                                         path_buf_size, out_precise, follow_active_view);
        case AST_UNSAFE_EXPR:
            return xa_root_path_for_expr(ctx, xa_unsafe_expr_result(expr), path_buf, path_buf_size,
                                         out_precise, follow_active_view);
        case AST_CALL_EXPR:
            if (xa_call_expr_preserves_owner_borrow(ctx, expr, NULL) && expr->as.call_expr.callee &&
                expr->as.call_expr.callee->type == AST_MEMBER_ACCESS) {
                MemberAccessNode *member = &expr->as.call_expr.callee->as.member_access;
                if (xa_call_expr_is_mem_view(ctx, expr))
                    return xa_root_path_for_expr(ctx, expr->as.call_expr.arguments[0], path_buf,
                                                 path_buf_size, out_precise, follow_active_view);
                if (member->name && strcmp(member->name, "offset") == 0 &&
                    !xa_pointer_expr_has_owner_borrow(ctx, member->object))
                    return NULL;
                return xa_root_path_for_expr(ctx, member->object, path_buf, path_buf_size,
                                             out_precise, follow_active_view);
            }
            return NULL;
        default:
            return NULL;
    }
}

static bool xa_node_uses_symbol_name(AstNode *node, const char *name);

static bool xa_block_node_statements(AstNode *node, AstNode ***out_statements, int *out_count) {
    if (out_statements)
        *out_statements = NULL;
    if (out_count)
        *out_count = 0;
    if (!node)
        return false;
    if (node->type == AST_BLOCK) {
        if (out_statements)
            *out_statements = node->as.block.statements;
        if (out_count)
            *out_count = node->as.block.count;
        return true;
    }
    if (node->type == AST_PROGRAM) {
        if (out_statements)
            *out_statements = node->as.program.statements;
        if (out_count)
            *out_count = node->as.program.count;
        return true;
    }
    return false;
}

static bool xa_node_array_uses_symbol_name(AstNode **nodes, int count, const char *name) {
    if (!nodes || count <= 0 || !name)
        return false;
    for (int i = 0; i < count; i++) {
        if (xa_node_uses_symbol_name(nodes[i], name))
            return true;
    }
    return false;
}

static bool xa_param_defaults_use_symbol_name(XrParamNode **params, int count, const char *name) {
    if (!params || count <= 0)
        return false;
    for (int i = 0; i < count; i++) {
        if (params[i] && xa_node_uses_symbol_name(params[i]->default_value, name))
            return true;
    }
    return false;
}

static bool xa_block_uses_symbol_name_from(AstNode *node, const char *name, int start_index) {
    if (!node || !name)
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(node, &statements, &count))
        return xa_node_uses_symbol_name(node, name);
    if (start_index < 0)
        start_index = 0;
    for (int i = start_index; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        if ((stmt->type == AST_VAR_DECL || stmt->type == AST_CONST_DECL ||
             stmt->type == AST_SHARED_DECL || stmt->type == AST_OWNED_DECL) &&
            stmt->as.var_decl.name && strcmp(stmt->as.var_decl.name, name) == 0) {
            return xa_node_uses_symbol_name(stmt->as.var_decl.initializer, name);
        }
        if (xa_node_uses_symbol_name(stmt, name))
            return true;
    }
    return false;
}

static bool xa_node_uses_symbol_name(AstNode *node, const char *name) {
    if (!node || !name)
        return false;

    switch (node->type) {
        case AST_VARIABLE:
            return node->as.variable.name && strcmp(node->as.variable.name, name) == 0;

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
        case AST_NULLISH_COALESCE:
            return xa_node_uses_symbol_name(node->as.binary.left, name) ||
                   xa_node_uses_symbol_name(node->as.binary.right, name);

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            return xa_node_uses_symbol_name(node->as.unary.operand, name);

        case AST_GROUPING:
            return xa_node_uses_symbol_name(node->as.grouping, name);

        case AST_EXPR_STMT:
            return xa_node_uses_symbol_name(node->as.expr_stmt, name);
        case AST_PRINT_STMT:
            return xa_node_array_uses_symbol_name(node->as.print_stmt.exprs,
                                                  node->as.print_stmt.expr_count, name);
        case AST_BLOCK:
            return xa_block_uses_symbol_name_from(node, name, 0);

        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            return xa_node_uses_symbol_name(node->as.var_decl.initializer, name);
        case AST_ASSIGNMENT:
            return xa_node_uses_symbol_name(node->as.assignment.value, name);
        case AST_COMPOUND_ASSIGNMENT:
            return (node->as.compound_assignment.name &&
                    strcmp(node->as.compound_assignment.name, name) == 0) ||
                   xa_node_uses_symbol_name(node->as.compound_assignment.object, name) ||
                   xa_node_uses_symbol_name(node->as.compound_assignment.value, name);
        case AST_INC:
        case AST_DEC:
            return node->as.inc.name && strcmp(node->as.inc.name, name) == 0;
        case AST_DESTRUCTURE_DECL:
            return xa_node_uses_symbol_name(node->as.destructure_decl.initializer, name);
        case AST_DESTRUCTURE_ASSIGN:
            return xa_node_uses_symbol_name(node->as.destructure_assign.value, name);

        case AST_IF_STMT:
            return xa_node_uses_symbol_name(node->as.if_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.if_stmt.then_branch, name) ||
                   xa_node_uses_symbol_name(node->as.if_stmt.else_branch, name);
        case AST_WHILE_STMT:
            return xa_node_uses_symbol_name(node->as.while_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.while_stmt.body, name);
        case AST_FOR_STMT:
            return xa_node_uses_symbol_name(node->as.for_stmt.initializer, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.increment, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.body, name);
        case AST_FOR_IN_STMT:
            return xa_node_uses_symbol_name(node->as.for_in_stmt.collection, name) ||
                   xa_node_uses_symbol_name(node->as.for_in_stmt.body, name);
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return xa_node_uses_symbol_name(node->as.function_decl.body, name);
        case AST_CALL_EXPR:
            return xa_node_uses_symbol_name(node->as.call_expr.callee, name) ||
                   xa_node_array_uses_symbol_name(node->as.call_expr.arguments,
                                                  node->as.call_expr.arg_count, name);
        case AST_RETURN_STMT:
            return xa_node_array_uses_symbol_name(node->as.return_stmt.values,
                                                  node->as.return_stmt.value_count, name);

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat)
                return xa_node_uses_symbol_name(node->as.array_literal.repeat_value, name) ||
                       xa_node_uses_symbol_name(node->as.array_literal.repeat_count, name);
            return xa_node_array_uses_symbol_name(node->as.array_literal.elements,
                                                  node->as.array_literal.count, name);
        case AST_TUPLE_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.tuple_literal.elements,
                                                  node->as.tuple_literal.count, name);
        case AST_SPREAD_EXPR:
            return xa_node_uses_symbol_name(node->as.spread_expr.expr, name);
        case AST_INDEX_GET:
            return xa_node_uses_symbol_name(node->as.index_get.array, name) ||
                   xa_node_uses_symbol_name(node->as.index_get.index, name);
        case AST_INDEX_SET:
            return xa_node_uses_symbol_name(node->as.index_set.array, name) ||
                   xa_node_uses_symbol_name(node->as.index_set.index, name) ||
                   xa_node_uses_symbol_name(node->as.index_set.value, name);
        case AST_SLICE_EXPR:
            return xa_node_uses_symbol_name(node->as.slice_expr.source, name) ||
                   xa_node_uses_symbol_name(node->as.slice_expr.start, name) ||
                   xa_node_uses_symbol_name(node->as.slice_expr.end, name);
        case AST_MEMBER_ACCESS:
            return xa_node_uses_symbol_name(node->as.member_access.object, name);
        case AST_MEMBER_SET:
            return xa_node_uses_symbol_name(node->as.member_set.object, name) ||
                   xa_node_uses_symbol_name(node->as.member_set.value, name);
        case AST_TEMPLATE_STRING:
            return xa_node_array_uses_symbol_name(node->as.template_str.parts,
                                                  node->as.template_str.part_count, name);
        case AST_OBJECT_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.object_literal.keys,
                                                  node->as.object_literal.count, name) ||
                   xa_node_array_uses_symbol_name(node->as.object_literal.values,
                                                  node->as.object_literal.count, name);
        case AST_MAP_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.map_literal.keys,
                                                  node->as.map_literal.count, name) ||
                   xa_node_array_uses_symbol_name(node->as.map_literal.values,
                                                  node->as.map_literal.count, name);
        case AST_SET_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.set_literal.elements,
                                                  node->as.set_literal.count, name);
        case AST_STRUCT_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.struct_literal.field_values,
                                                  node->as.struct_literal.field_count, name);

        case AST_TERNARY:
            return xa_node_uses_symbol_name(node->as.ternary.condition, name) ||
                   xa_node_uses_symbol_name(node->as.ternary.true_expr, name) ||
                   xa_node_uses_symbol_name(node->as.ternary.false_expr, name);
        case AST_OPTIONAL_CHAIN:
            return xa_node_uses_symbol_name(node->as.optional_chain.object, name) ||
                   xa_node_uses_symbol_name(node->as.optional_chain.index, name);
        case AST_RANGE:
            return xa_node_uses_symbol_name(node->as.range.start, name) ||
                   xa_node_uses_symbol_name(node->as.range.end, name);
        case AST_IS_EXPR:
            return xa_node_uses_symbol_name(node->as.is_expr.expr, name);
        case AST_AS_EXPR:
            return xa_node_uses_symbol_name(node->as.as_expr.expr, name);
        case AST_NEW_EXPR:
            return xa_node_array_uses_symbol_name(node->as.new_expr.arguments,
                                                  node->as.new_expr.arg_count, name);
        case AST_SUPER_CALL:
            return xa_node_array_uses_symbol_name(node->as.super_call.arguments,
                                                  node->as.super_call.arg_count, name);

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            return xa_node_array_uses_symbol_name(node->as.class_decl.fields,
                                                  node->as.class_decl.field_count, name) ||
                   xa_node_array_uses_symbol_name(node->as.class_decl.methods,
                                                  node->as.class_decl.method_count, name);
        case AST_FIELD_DECL:
            return xa_node_uses_symbol_name(node->as.field_decl.initializer, name);
        case AST_METHOD_DECL:
            return xa_node_array_uses_symbol_name(node->as.method_decl.base_args,
                                                  node->as.method_decl.base_arg_count, name) ||
                   xa_param_defaults_use_symbol_name(node->as.method_decl.params,
                                                     node->as.method_decl.param_count, name) ||
                   xa_node_uses_symbol_name(node->as.method_decl.body, name);

        case AST_ENUM_DECL:
            return xa_node_array_uses_symbol_name(node->as.enum_decl.members,
                                                  node->as.enum_decl.member_count, name) ||
                   xa_node_array_uses_symbol_name(node->as.enum_decl.methods,
                                                  node->as.enum_decl.method_count, name);
        case AST_ENUM_MEMBER:
            return false;
        case AST_ENUM_INDEX:
            return xa_node_uses_symbol_name(node->as.enum_index.collection, name) ||
                   xa_node_uses_symbol_name(node->as.enum_index.index_expr, name);

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (xa_node_uses_symbol_name(tc->try_body, name))
                return true;
            for (int i = 0; i < tc->catch_count; i++) {
                if (tc->catch_clauses[i] &&
                    xa_node_uses_symbol_name(tc->catch_clauses[i]->body, name))
                    return true;
            }
            return false;
        }
        case AST_THROW_STMT:
            return xa_node_uses_symbol_name(node->as.throw_stmt.expression, name);
        case AST_EXPORT_STMT:
            return xa_node_uses_symbol_name(node->as.export_stmt.declaration, name);
        case AST_GLOBAL_ASM:
            return false;

        case AST_MATCH_EXPR:
            return xa_node_uses_symbol_name(node->as.match_expr.expr, name) ||
                   xa_node_array_uses_symbol_name(node->as.match_expr.arms,
                                                  node->as.match_expr.arm_count, name);
        case AST_MATCH_ARM:
            return xa_node_uses_symbol_name(node->as.match_arm.pattern, name) ||
                   xa_node_uses_symbol_name(node->as.match_arm.guard, name) ||
                   xa_node_uses_symbol_name(node->as.match_arm.body, name);
        case AST_PATTERN_LITERAL:
            return xa_node_uses_symbol_name(node->as.pattern_literal.value, name);
        case AST_PATTERN_RANGE:
            return xa_node_uses_symbol_name(node->as.pattern_range.start, name) ||
                   xa_node_uses_symbol_name(node->as.pattern_range.end, name);
        case AST_PATTERN_MULTI:
            return xa_node_array_uses_symbol_name(node->as.pattern_multi.patterns,
                                                  node->as.pattern_multi.count, name);
        case AST_PATTERN_TUPLE:
            return xa_node_array_uses_symbol_name(node->as.pattern_tuple.patterns,
                                                  node->as.pattern_tuple.count, name);
        case AST_PATTERN_ADT:
            return xa_node_uses_symbol_name(node->as.pattern_adt.variant, name) ||
                   xa_node_array_uses_symbol_name(node->as.pattern_adt.patterns,
                                                  node->as.pattern_adt.count, name);
        case AST_PATTERN_OBJECT:
            return xa_node_array_uses_symbol_name(node->as.pattern_object.patterns,
                                                  node->as.pattern_object.count, name);
        case AST_PATTERN_ARRAY:
            return xa_node_array_uses_symbol_name(node->as.pattern_array.patterns,
                                                  node->as.pattern_array.count, name);

        case AST_GO_EXPR:
            return xa_node_uses_symbol_name(node->as.go_expr.expr, name);
        case AST_AWAIT_EXPR:
            return xa_node_uses_symbol_name(node->as.await_expr.expr, name) ||
                   xa_node_uses_symbol_name(node->as.await_expr.timeout, name) ||
                   xa_node_uses_symbol_name(node->as.await_expr.into, name);
        case AST_CHANNEL_NEW:
            return xa_node_uses_symbol_name(node->as.channel_new.buffer_size, name);
        case AST_SELECT_STMT:
            return xa_node_array_uses_symbol_name(node->as.select_stmt.cases,
                                                  node->as.select_stmt.case_count, name);
        case AST_SELECT_CASE:
            return xa_node_uses_symbol_name(node->as.select_case.channel, name) ||
                   xa_node_uses_symbol_name(node->as.select_case.value, name) ||
                   xa_node_uses_symbol_name(node->as.select_case.body, name);
        case AST_YIELD_STMT:
            return xa_node_uses_symbol_name(node->as.yield_stmt.value, name);
        case AST_DEFER_STMT:
            return xa_node_uses_symbol_name(node->as.defer_stmt.expr, name);
        case AST_SCOPE_BLOCK:
            return xa_node_uses_symbol_name(node->as.scope_block.body, name);
        case AST_MOVE_EXPR:
            return xa_node_uses_symbol_name(node->as.move_expr.expr, name);
        case AST_COMPTIME_EXPR:
            return xa_node_uses_symbol_name(node->as.comptime_expr.expr, name);
        case AST_UNSAFE_EXPR:
            return xa_node_uses_symbol_name(node->as.unsafe_expr.operand, name);
        case AST_PROGRAM:
            return xa_node_array_uses_symbol_name(node->as.program.statements,
                                                  node->as.program.count, name);

        default:
            return false;
    }
}

static bool xa_active_span_borrow_may_be_live_after_mutation(XaInferContext *ctx,
                                                             XaActiveSpanBorrow *borrow) {
    if (!ctx || !ctx->analyzer || !borrow || !borrow->view_symbol || !borrow->view_symbol->name)
        return true;
    if (ctx->loop_depth > 0 && borrow->loop_depth_at_creation < ctx->loop_depth)
        return true;
    const char *name = borrow->view_symbol->name;
    if (ctx->block_cursor_depth > 0) {
        int deepest = ctx->block_cursor_depth - 1;
        for (int depth = deepest; depth >= 0; depth--) {
            AstNode *block_node = ctx->block_cursor_nodes[depth];
            int stmt_index = ctx->block_cursor_indices[depth];
            AstNode **statements = NULL;
            int count = 0;
            if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
                stmt_index >= count)
                return true;
            if (depth == deepest && xa_node_uses_symbol_name(statements[stmt_index], name))
                return true;
            if (xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1))
                return true;
        }
        return false;
    }

    AstNode *block_node = ctx->current_block_node;
    int stmt_index = ctx->current_block_stmt_index;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
        stmt_index >= count)
        return true;
    if (xa_node_uses_symbol_name(statements[stmt_index], name))
        return true;
    return xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1);
}

XR_FUNC void xa_visit_inline_statement_sequence_with_cursor(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;
    AstNode **statements = NULL;
    int count = 0;
    if (node->type == AST_BLOCK) {
        statements = node->as.block.statements;
        count = node->as.block.count;
    } else if (node->type == AST_PROGRAM) {
        statements = node->as.program.statements;
        count = node->as.program.count;
    } else {
        xa_visit_infer_stmt(ctx, node);
        return;
    }

    AstNode *saved_block = ctx->current_block_node;
    int saved_index = ctx->current_block_stmt_index;
    int saved_depth = ctx->block_cursor_depth;
    int cursor_slot = -1;
    if (ctx->block_cursor_depth < XA_BLOCK_CURSOR_MAX) {
        cursor_slot = ctx->block_cursor_depth++;
        ctx->block_cursor_nodes[cursor_slot] = node;
        ctx->block_cursor_indices[cursor_slot] = -1;
    }
    ctx->current_block_node = node;
    int error_count_before = xa_analyzer_error_diagnostic_count(ctx->analyzer);
    for (int i = 0; i < count; i++) {
        ctx->current_block_stmt_index = i;
        if (cursor_slot >= 0)
            ctx->block_cursor_indices[cursor_slot] = i;
        xa_visit_infer_stmt(ctx, statements[i]);
    }
    xa_warn_sys_thread_lifecycle_in_sequence(ctx, statements, count, error_count_before);
    xa_warn_os_resource_lifecycle_in_sequence(ctx, statements, count, error_count_before);
    ctx->block_cursor_depth = saved_depth;
    ctx->current_block_node = saved_block;
    ctx->current_block_stmt_index = saved_index;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_receiver_symbol(XaInferContext *ctx, AstNode *expr,
                                                       XrType *receiver_type) {
    if (!ctx || !xa_type_can_own_span_view(receiver_type))
        return NULL;
    return xa_root_variable_symbol_for_expr(ctx, expr);
}

XR_FUNC void xa_clear_active_span_borrow_for_view(XaInferContext *ctx, XaSymbol *view_sym) {
    if (!ctx || !view_sym)
        return;
    XaActiveSpanBorrow **link = &ctx->active_span_borrows;
    while (*link) {
        XaActiveSpanBorrow *cur = *link;
        if (cur->view_symbol == view_sym) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

XR_FUNC void xa_clear_active_span_borrows_in_scope(XaInferContext *ctx, XaScope *scope) {
    if (!ctx || !scope)
        return;
    XaActiveSpanBorrow **link = &ctx->active_span_borrows;
    while (*link) {
        XaActiveSpanBorrow *cur = *link;
        if (cur->view_scope == scope) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_expr(XaInferContext *ctx, AstNode *expr,
                                                     char *path_buf, size_t path_buf_size) {
    if (!ctx || !expr)
        return NULL;
    char local_path[512];
    if (!path_buf || path_buf_size == 0) {
        path_buf = local_path;
        path_buf_size = sizeof(local_path);
    }
    path_buf[0] = '\0';

    XrType *expr_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (xa_type_can_own_span_view(expr_type) || xa_type_contains_span_view(expr_type) ||
        (expr_type && XR_TYPE_IS_POINTER(expr_type))) {
        bool precise = true;
        XaSymbol *root = xa_root_path_for_expr(ctx, expr, path_buf, path_buf_size, &precise, true);
        if (root)
            return root;
    }
    return NULL;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_owner_expr(XaInferContext *ctx, AstNode *expr,
                                                           char *path_buf, size_t path_buf_size) {
    if (!ctx || !expr)
        return NULL;
    char local_path[512];
    if (!path_buf || path_buf_size == 0) {
        path_buf = local_path;
        path_buf_size = sizeof(local_path);
    }
    path_buf[0] = '\0';

    XrType *expr_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (!xa_type_can_own_span_view(expr_type))
        return NULL;
    bool precise = true;
    return xa_root_path_for_expr(ctx, expr, path_buf, path_buf_size, &precise, false);
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_member_write(XaInferContext *ctx, AstNode *object,
                                                             const char *member,
                                                             XrType *member_type, char *path_buf,
                                                             size_t path_buf_size) {
    if (!ctx || !object || !member || !xa_type_can_own_span_view(member_type) || !path_buf ||
        path_buf_size == 0)
        return NULL;
    path_buf[0] = '\0';
    bool precise = true;
    XaSymbol *root = xa_root_path_for_expr(ctx, object, path_buf, path_buf_size, &precise, true);
    if (!root)
        return NULL;
    if (precise) {
        if (!xa_path_append(path_buf, path_buf_size, ".") ||
            !xa_path_append(path_buf, path_buf_size, member)) {
            xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
        }
    } else {
        xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    }
    return root;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_index_write(XaInferContext *ctx, AstNode *array,
                                                            AstNode *index, XrType *element_type,
                                                            char *path_buf, size_t path_buf_size) {
    if (!ctx || !array || !xa_type_can_own_span_view(element_type) || !path_buf ||
        path_buf_size == 0)
        return NULL;
    path_buf[0] = '\0';
    bool precise = true;
    XaSymbol *root = xa_root_path_for_expr(ctx, array, path_buf, path_buf_size, &precise, true);
    if (!root)
        return NULL;
    char segment[64];
    if (precise && xa_index_path_segment(index, segment, sizeof(segment))) {
        if (!xa_path_append(path_buf, path_buf_size, segment))
            xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    } else {
        xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    }
    return root;
}

XR_FUNC void xa_register_active_span_borrow(XaInferContext *ctx, XaSymbol *view_sym, AstNode *value,
                                            XrType *value_type) {
    if (!ctx || !view_sym)
        return;
    xa_clear_active_span_borrow_for_view(ctx, view_sym);
    bool is_pointer_borrow = false;
    bool is_span_borrow = xa_type_contains_span_view(value_type);
    if (!is_span_borrow && value_type && XR_TYPE_IS_POINTER(value_type)) {
        AstNode *source = value;
        while (source && (source->type == AST_GROUPING || source->type == AST_UNSAFE_EXPR ||
                          source->type == AST_AS_EXPR)) {
            if (source->type == AST_GROUPING)
                source = source->as.grouping;
            else if (source->type == AST_UNSAFE_EXPR)
                source = xa_unsafe_expr_result(source);
            else
                source = source->as.as_expr.expr;
        }
        is_pointer_borrow = xa_call_expr_preserves_owner_borrow(ctx, source, NULL) ||
                            xa_pointer_expr_has_owner_borrow(ctx, source);
    }
    if (!value || (!is_span_borrow && !is_pointer_borrow))
        return;
    char owner_path[512];
    XaSymbol *owner =
        xa_span_borrow_owner_path_for_expr(ctx, value, owner_path, sizeof(owner_path));
    if (!owner || owner == view_sym)
        return;
    XaActiveSpanBorrow *borrow = xr_calloc(1, sizeof(XaActiveSpanBorrow));
    if (!borrow)
        return;
    borrow->owner_symbol = owner;
    if (owner_path[0] != '\0')
        borrow->owner_path = xr_strdup(owner_path);
    borrow->view_symbol = view_sym;
    borrow->view_scope = view_sym->scope;
    borrow->loop_depth_at_creation = ctx->loop_depth;
    borrow->is_pointer_borrow = is_pointer_borrow;
    borrow->next = ctx->active_span_borrows;
    ctx->active_span_borrows = borrow;
}

XR_FUNC void xa_check_active_span_borrow_owner_path_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                             XaSymbol *owner_sym,
                                                             const char *owner_path,
                                                             const char *operation) {
    if (!ctx || !ctx->analyzer || !owner_sym || !loc_node)
        return;
    for (XaActiveSpanBorrow *b = ctx->active_span_borrows; b; b = b->next) {
        if (b->owner_symbol != owner_sym)
            continue;
        if (!xa_owner_paths_may_overlap(b->owner_path, owner_path))
            continue;
        if (!xa_active_span_borrow_may_be_live_after_mutation(ctx, b))
            continue;
        XrLocation loc = {
            .file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
        char msg[256];
        if (b->is_pointer_borrow) {
            snprintf(msg, sizeof(msg),
                     "cannot mutate owner '%s' while raw pointer borrow '%s' is active; end the "
                     "borrow scope before %s",
                     owner_sym->name ? owner_sym->name : "?",
                     b->view_symbol && b->view_symbol->name ? b->view_symbol->name : "?",
                     operation ? operation : "mutating the owner");
        } else {
            snprintf(msg, sizeof(msg),
                     "cannot mutate owner '%s' while Slice view '%s' is active; end the view scope "
                     "before %s",
                     owner_sym->name ? owner_sym->name : "?",
                     b->view_symbol && b->view_symbol->name ? b->view_symbol->name : "?",
                     operation ? operation : "mutating the owner");
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return;
    }
}

XR_FUNC void xa_check_active_span_borrow_owner_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                        XaSymbol *owner_sym,
                                                        const char *operation) {
    const char *owner_path = owner_sym && owner_sym->name ? owner_sym->name : NULL;
    xa_check_active_span_borrow_owner_path_mutation(ctx, loc_node, owner_sym, owner_path,
                                                    operation);
}

XR_FUNC void xa_check_span_value_escape(XaInferContext *ctx, AstNode *loc_node, XrType *value_type,
                                        const char *escape_context) {
    if (!ctx || !ctx->analyzer || !loc_node || !xa_type_contains_span_view(value_type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot %s; Slice is a borrowed view, keep it local or copy the owner data into an "
             "Array",
             escape_context ? escape_context : "var Slice view escape");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

XR_FUNC void xa_check_span_generic_class_type_args(XaInferContext *ctx, AstNode *loc_node,
                                                   const char *class_name, XrType **type_args,
                                                   int type_arg_count) {
    if (!ctx || !loc_node || !type_args || type_arg_count <= 0)
        return;

    for (int i = 0; i < type_arg_count; i++) {
        if (!xa_type_contains_span_view(type_args[i]))
            continue;
        char context[160];
        if (class_name && class_name[0]) {
            snprintf(context, sizeof(context),
                     "use Slice view as generic class/struct type argument for '%s'", class_name);
        } else {
            snprintf(context, sizeof(context),
                     "use Slice view as generic class/struct type argument");
        }
        xa_check_span_value_escape(ctx, loc_node, type_args[i], context);
        return;
    }
}

XR_FUNC void xa_check_span_borrow_source_stable(XaInferContext *ctx, AstNode *loc_node,
                                                AstNode *source, const char *operation) {
    if (!ctx || !ctx->analyzer || !loc_node || !source || xa_expr_has_stable_borrow_owner(source))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
    char msg[256];
    snprintf(
        msg, sizeof(msg),
        "cannot create Slice view from temporary owner in %s; bind the owner to a local before "
        "borrowing it",
        operation ? operation : "borrow expression");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_update_borrowed_alias_root(XaInferContext *ctx, XaSymbol *sym, AstNode *value,
                                          XrType *value_type) {
    if (!sym || sym->kind != XA_SYM_VARIABLE)
        return;
    sym->borrowed_root_symbol_id = 0;
    if (!value || !xa_type_needs_borrow_escape_guard(value_type))
        return;
    XaSymbol *root = xa_borrowed_param_root_symbol(ctx, value);
    if (root)
        sym->borrowed_root_symbol_id = root->id;
}

static void xa_check_borrowed_return_escape(XaInferContext *ctx, AstNode *return_node,
                                            AstNode *value, XrType *value_type) {
    if (!ctx || !return_node || !value || !xa_type_needs_borrow_escape_guard(value_type))
        return;
    XaSymbol *root = xa_borrowed_param_root_symbol(ctx, value);
    if (!root)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = value->line ? value->line : return_node->line,
                      .column = value->column ? value->column : return_node->column};
    const char *mode = root->passing_mode == XR_PARAM_REF ? "ref" : "in";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot return borrowed '%s' parameter '%s'; return an owned value or copy(%s)", mode,
             root->name ? root->name : "?", root->name ? root->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_span_return_escape(XaInferContext *ctx, AstNode *return_node,
                                        XrType *value_type) {
    if (!ctx || !return_node || !xa_type_contains_span_view(value_type))
        return;

    XrLocation loc = {
        .file = ctx->file_path, .line = return_node->line, .column = return_node->column};
    xa_analyzer_add_diagnostic(
        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
        "cannot return Slice view; return the owner container or copy the view into an Array",
        &loc);
}

static bool xa_call_is_copy_builtin(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    return call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
           strcmp(call->callee->as.variable.name, "copy") == 0 && call->arg_count == 1;
}

static void xa_ensure_function_return_storage_prepass(XaInferContext *ctx, XaSymbolLinks *links);
static void xa_ensure_function_return_function_effect_prepass(XaInferContext *ctx,
                                                              XaSymbolLinks *links);
static AstNode *xa_storage_boundary_identity_source(AstNode *expr);

static bool xa_call_return_storage_summary(XaInferContext *ctx, AstNode *node, uint8_t *owner_out,
                                           bool *mixed_out, const char **name_out) {
    if (owner_out)
        *owner_out = XR_STORAGE_NONE;
    if (mixed_out)
        *mixed_out = false;
    if (name_out)
        *name_out = NULL;
    if (!ctx || !node || node->type != AST_CALL_EXPR)
        return false;

    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE || !call->callee->as.variable.name)
        return false;

    XaSymbol *sym = xa_lookup_visible_symbol(ctx, call->callee->as.variable.name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    if (links && !links->return_storage_known && !links->return_storage_mixed)
        xa_ensure_function_return_storage_prepass(ctx, links);
    if (!links)
        return false;

    if (links->return_storage_mixed) {
        if (mixed_out)
            *mixed_out = true;
        if (name_out)
            *name_out = call->callee->as.variable.name;
        return true;
    }

    if (!links->return_storage_known)
        return false;

    if (owner_out)
        *owner_out = links->return_storage_owner;
    if (name_out)
        *name_out = call->callee->as.variable.name;
    return true;
}

static bool xa_call_return_storage_owner(XaInferContext *ctx, AstNode *node, uint8_t *owner_out,
                                         const char **name_out) {
    bool mixed = false;
    if (!xa_call_return_storage_summary(ctx, node, owner_out, &mixed, name_out) || mixed)
        return false;
    return true;
}

static AstNode *xa_direct_function_value_source(AstNode *expr) {
    AstNode *source = xa_storage_boundary_identity_source(expr);
    if (!source)
        return NULL;
    return (source->type == AST_VARIABLE || source->type == AST_MEMBER_ACCESS) ? source : NULL;
}

static XaSymbol *xa_function_value_source_symbol(XaInferContext *ctx, AstNode *source) {
    if (!ctx || !ctx->analyzer || !source)
        return NULL;
    if (source->type == AST_VARIABLE)
        return source->as.variable.name ? xa_lookup_visible_symbol(ctx, source->as.variable.name)
                                        : NULL;
    if (source->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &source->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE ||
        !ma->object->as.variable.name)
        return NULL;
    XaSymbol *mod_sym = xa_scope_lookup(ctx->analyzer->current_scope, ma->object->as.variable.name);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *mod_links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *mod_name = (mod_links && mod_links->module_name) ? mod_links->module_name
                                                                 : ma->object->as.variable.name;
    bool is_quoted = mod_name && (mod_name[0] == '.' || mod_name[0] == '/');
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    XaSymbol *member_sym = exports ? (XaSymbol *) xr_hashmap_get(exports, ma->name) : NULL;
    return member_sym && member_sym->kind == XA_SYM_FUNCTION ? member_sym : NULL;
}

static XrType *xa_links_function_return_type(XaSymbolLinks *links) {
    if (!links)
        return NULL;
    if (links->return_type)
        return links->return_type;
    if (links->type && XR_TYPE_IS_FUNCTION(links->type))
        return links->type->function.return_type;
    return NULL;
}

static bool xa_links_return_function_effect_is_ready(XaSymbolLinks *links) {
    XrType *return_type = xa_links_function_return_type(links);
    if (!return_type || !XR_TYPE_IS_FUNCTION(return_type))
        return false;
    int param_count = return_type->function.param_count;
    if (param_count <= 0)
        return true;
    return links && links->return_fn_param_mutations &&
           links->return_fn_param_mutation_count >= param_count;
}

static bool xa_param_effect_summary_is_ready(XaSymbolLinks *links) {
    if (!links)
        return false;
    int param_count = links->param_count;
    if (param_count <= 0 && links->type && XR_TYPE_IS_FUNCTION(links->type))
        param_count = links->type->function.param_count;
    if (param_count <= 0)
        return true;
    return links->param_mutations && links->param_mutation_count >= param_count;
}

static bool xa_u8_summary_equal(const uint8_t *a, int a_count, const uint8_t *b, int b_count) {
    if (a_count != b_count)
        return false;
    if (a_count <= 0)
        return true;
    if (!a || !b)
        return false;
    return memcmp(a, b, (size_t) a_count * sizeof(uint8_t)) == 0;
}

static bool xa_return_function_effect_matches_param_summary(XaSymbolLinks *dst,
                                                            XaSymbolLinks *src) {
    if (!dst || !src)
        return false;
    return xa_u8_summary_equal(dst->return_fn_param_escapes, dst->return_fn_param_escape_count,
                               src->param_escapes, src->param_escape_count) &&
           xa_u8_summary_equal(dst->return_fn_param_mutations, dst->return_fn_param_mutation_count,
                               src->param_mutations, src->param_mutation_count) &&
           xa_u8_summary_equal(dst->return_fn_param_storage_requirements,
                               dst->return_fn_param_storage_requirement_count,
                               src->param_storage_requirements,
                               src->param_storage_requirement_count);
}

static bool xa_return_function_effect_matches_return_summary(XaSymbolLinks *dst,
                                                             XaSymbolLinks *src) {
    if (!dst || !src)
        return false;
    return xa_u8_summary_equal(dst->return_fn_param_escapes, dst->return_fn_param_escape_count,
                               src->return_fn_param_escapes, src->return_fn_param_escape_count) &&
           xa_u8_summary_equal(dst->return_fn_param_mutations, dst->return_fn_param_mutation_count,
                               src->return_fn_param_mutations,
                               src->return_fn_param_mutation_count) &&
           xa_u8_summary_equal(dst->return_fn_param_storage_requirements,
                               dst->return_fn_param_storage_requirement_count,
                               src->return_fn_param_storage_requirements,
                               src->return_fn_param_storage_requirement_count);
}

static XaSymbolLinks *xa_call_return_function_effect_links(XaInferContext *ctx, AstNode *node,
                                                           const char **name_out) {
    if (name_out)
        *name_out = NULL;
    AstNode *direct = xa_storage_boundary_identity_source(node);
    if (!ctx || !direct || direct->type != AST_CALL_EXPR)
        return NULL;

    CallExprNode *call = &direct->as.call_expr;
    AstNode *callee = xa_direct_function_value_source(call->callee);
    XaSymbol *sym = xa_function_value_source_symbol(ctx, callee);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    XrType *return_type = xa_links_function_return_type(links);
    if (!return_type || !XR_TYPE_IS_FUNCTION(return_type))
        return NULL;
    if (name_out && callee && callee->type == AST_VARIABLE)
        *name_out = callee->as.variable.name;
    if (links && !links->return_fn_effect_scanned && !links->return_fn_effect_scan_in_progress)
        xa_ensure_function_return_function_effect_prepass(ctx, links);
    return links;
}

static void xa_propagate_function_value_summary(XaInferContext *ctx, XaSymbol *dst_sym,
                                                AstNode *initializer, XrType *init_type) {
    if (!ctx || !ctx->analyzer || !dst_sym || !initializer || !XR_TYPE_IS_FUNCTION(init_type))
        return;
    if (!dst_sym->is_const)
        return;

    XaSymbolLinks *dst_links = xa_analyzer_get_links(ctx->analyzer, dst_sym);
    if (!dst_links)
        return;

    AstNode *source = xa_direct_function_value_source(initializer);
    if (source) {
        XaSymbol *src_sym = xa_function_value_source_symbol(ctx, source);
        if (!src_sym)
            return;
        XaSymbolLinks *src_links = xa_analyzer_get_links(ctx->analyzer, src_sym);
        if (!src_links)
            return;

        xa_symbol_links_copy_param_effect_summaries(dst_links, src_links);

        if (src_sym->kind == XA_SYM_FUNCTION && !src_links->return_storage_scanned &&
            !src_links->return_storage_scan_in_progress)
            xa_ensure_function_return_storage_prepass(ctx, src_links);
        if (src_links->return_storage_scanned) {
            dst_links->return_storage_owner = src_links->return_storage_owner;
            dst_links->return_storage_known = src_links->return_storage_known;
            dst_links->return_storage_mixed = src_links->return_storage_mixed;
            dst_links->return_storage_scanned = true;
            dst_links->return_storage_scan_in_progress = false;
        }

        if (src_sym->kind == XA_SYM_FUNCTION && !src_links->return_fn_effect_scanned &&
            !src_links->return_fn_effect_scan_in_progress)
            xa_ensure_function_return_function_effect_prepass(ctx, src_links);
        if (src_links->return_fn_effect_scanned)
            xa_symbol_links_copy_return_function_effect_summary(dst_links, src_links);
        return;
    }

    XaSymbolLinks *callee_links = xa_call_return_function_effect_links(ctx, initializer, NULL);
    if (!callee_links || !callee_links->return_fn_effect_scanned ||
        callee_links->return_fn_effect_mixed ||
        !xa_links_return_function_effect_is_ready(callee_links))
        return;
    xa_symbol_links_copy_return_function_effect_to_param_summaries(dst_links, callee_links);
}

static bool xa_call_is_unknown_storage_function_value(XaInferContext *ctx, AstNode *node,
                                                      const char **name_out) {
    if (name_out)
        *name_out = NULL;
    AstNode *init = xa_storage_boundary_identity_source(node);
    if (!ctx || !init || init->type != AST_CALL_EXPR)
        return false;

    CallExprNode *call = &init->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE || !call->callee->as.variable.name)
        return false;

    XaSymbol *sym = xa_lookup_visible_symbol(ctx, call->callee->as.variable.name);
    if (!sym || sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_IMPORT)
        return false;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    XrType *callee_type =
        links && links->type ? links->type : xa_analyzer_get_type(ctx->analyzer, sym);
    if (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type))
        return false;
    XrType *return_type = callee_type->function.return_type;
    if (!return_type || XR_TYPE_IS_UNKNOWN(return_type) ||
        return_type->kind == XR_KIND_TYPE_PARAM || !xa_type_needs_borrow_escape_guard(return_type))
        return false;
    if (links && links->return_storage_scanned)
        return false;

    if (name_out)
        *name_out = call->callee->as.variable.name;
    return true;
}

static AstNode *xa_storage_boundary_identity_source(AstNode *expr) {
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

static AstNode *xa_shared_boundary_source(AstNode *init, bool *is_move) {
    if (is_move)
        *is_move = false;
    init = xa_storage_boundary_identity_source(init);
    if (!init || xa_call_is_copy_builtin(init))
        return NULL;
    if (init->type == AST_MOVE_EXPR) {
        if (is_move)
            *is_move = true;
        AstNode *inner = xa_storage_boundary_identity_source(init->as.move_expr.expr);
        return (inner && inner->type == AST_VARIABLE) ? inner : NULL;
    }
    return init->type == AST_VARIABLE ? init : NULL;
}

typedef struct XaReturnStoragePrepass {
    XaInferContext *ctx;
    const char *names[256];
    uint8_t owners[256];
    int count;
    uint8_t owner;
    bool known;
    bool mixed;
    bool unknown;
    bool use_scope_lookup;
} XaReturnStoragePrepass;

static void xa_return_storage_prepass_record_owner(XaReturnStoragePrepass *scan, uint8_t owner) {
    if (!scan)
        return;
    if (owner != XR_STORAGE_OWNED_SYSTEM && owner != XR_STORAGE_SHARED_SYSTEM)
        return;
    if (!scan->known) {
        scan->owner = owner;
        scan->known = true;
        if (scan->unknown)
            scan->mixed = true;
        return;
    }
    if (scan->owner != owner)
        scan->mixed = true;
}

static void xa_return_storage_prepass_record_unknown(XaReturnStoragePrepass *scan) {
    if (!scan)
        return;
    if (scan->known)
        scan->mixed = true;
    scan->unknown = true;
}

static void xa_return_storage_prepass_bind(XaReturnStoragePrepass *scan, const char *name,
                                           uint8_t owner) {
    if (!scan || !name)
        return;
    if (scan->count >= (int) (sizeof(scan->names) / sizeof(scan->names[0]))) {
        xa_return_storage_prepass_record_unknown(scan);
        return;
    }
    scan->names[scan->count] = name;
    scan->owners[scan->count] = owner;
    scan->count++;
}

static uint8_t xa_return_storage_prepass_lookup(XaReturnStoragePrepass *scan, const char *name) {
    if (!scan || !name)
        return XR_STORAGE_NONE;
    for (int i = scan->count - 1; i >= 0; i--) {
        if (scan->names[i] && strcmp(scan->names[i], name) == 0)
            return scan->owners[i];
    }
    if (scan->use_scope_lookup && scan->ctx) {
        XaSymbol *sym = xa_lookup_visible_symbol(scan->ctx, name);
        if (sym && sym->kind == XA_SYM_VARIABLE) {
            if (sym->is_owned)
                return XR_STORAGE_OWNED_SYSTEM;
            if (sym->is_shared)
                return XR_STORAGE_SHARED_SYSTEM;
        }
    }
    return XR_STORAGE_NONE;
}

static bool xa_return_storage_prepass_expr_summary(XaReturnStoragePrepass *scan, AstNode *expr,
                                                   uint8_t *owner_out, bool *mixed_out);

static void xa_return_storage_prepass_accumulate_expr_summary(XaReturnStoragePrepass *scan,
                                                              AstNode *expr, bool *any_known,
                                                              bool *any_unknown, uint8_t *owner_io,
                                                              bool *mixed_io) {
    if (!any_known || !any_unknown || !owner_io || !mixed_io)
        return;

    uint8_t arm_owner = XR_STORAGE_NONE;
    bool arm_mixed = false;
    if (!xa_return_storage_prepass_expr_summary(scan, expr, &arm_owner, &arm_mixed)) {
        *any_unknown = true;
        return;
    }
    if (arm_mixed) {
        *any_known = true;
        *mixed_io = true;
        return;
    }
    if (arm_owner != XR_STORAGE_OWNED_SYSTEM && arm_owner != XR_STORAGE_SHARED_SYSTEM) {
        *any_unknown = true;
        return;
    }
    if (!*any_known) {
        *owner_io = arm_owner;
        *any_known = true;
        return;
    }
    if (*owner_io != arm_owner)
        *mixed_io = true;
}

static bool xa_return_storage_prepass_finish_branch_summary(bool any_known, bool any_unknown,
                                                            uint8_t owner, bool mixed,
                                                            uint8_t *owner_out, bool *mixed_out) {
    if (mixed || (any_known && any_unknown)) {
        if (mixed_out)
            *mixed_out = true;
        return true;
    }
    if (!any_known)
        return false;
    if (owner_out)
        *owner_out = owner;
    return true;
}

static AstNode *xa_return_storage_prepass_match_arm_value(AstNode *arm) {
    if (!arm || arm->type != AST_MATCH_ARM)
        return NULL;
    AstNode *body = arm->as.match_arm.body;
    if (body && (body->type == AST_BLOCK || body->type == AST_PROGRAM))
        body = xa_lifecycle_lint_single_expr_block_value(body);
    return body;
}

static bool xa_return_storage_prepass_expr_summary(XaReturnStoragePrepass *scan, AstNode *expr,
                                                   uint8_t *owner_out, bool *mixed_out) {
    if (owner_out)
        *owner_out = XR_STORAGE_NONE;
    if (mixed_out)
        *mixed_out = false;

    AstNode *block_value = NULL;
    if (expr && (expr->type == AST_BLOCK || expr->type == AST_PROGRAM) &&
        (block_value = xa_lifecycle_lint_single_expr_block_value(expr)))
        return xa_return_storage_prepass_expr_summary(scan, block_value, owner_out, mixed_out);

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(expr, &is_move);
    if (source && source->type == AST_VARIABLE && source->as.variable.name) {
        uint8_t owner = xa_return_storage_prepass_lookup(scan, source->as.variable.name);
        if (is_move && owner == XR_STORAGE_OWNED_SYSTEM) {
            if (owner_out)
                *owner_out = owner;
            return true;
        }
        if (!is_move && owner == XR_STORAGE_SHARED_SYSTEM) {
            if (owner_out)
                *owner_out = owner;
            return true;
        }
        return false;
    }

    AstNode *direct = xa_storage_boundary_identity_source(expr);
    if (direct && direct->type == AST_TERNARY) {
        bool any_known = false;
        bool any_unknown = false;
        uint8_t owner = XR_STORAGE_NONE;
        bool mixed = false;
        xa_return_storage_prepass_accumulate_expr_summary(scan, direct->as.ternary.true_expr,
                                                          &any_known, &any_unknown, &owner, &mixed);
        xa_return_storage_prepass_accumulate_expr_summary(scan, direct->as.ternary.false_expr,
                                                          &any_known, &any_unknown, &owner, &mixed);
        return xa_return_storage_prepass_finish_branch_summary(any_known, any_unknown, owner, mixed,
                                                               owner_out, mixed_out);
    }
    if (direct && direct->type == AST_NULLISH_COALESCE) {
        bool any_known = false;
        bool any_unknown = false;
        uint8_t owner = XR_STORAGE_NONE;
        bool mixed = false;
        xa_return_storage_prepass_accumulate_expr_summary(scan, direct->as.binary.left, &any_known,
                                                          &any_unknown, &owner, &mixed);
        xa_return_storage_prepass_accumulate_expr_summary(scan, direct->as.binary.right, &any_known,
                                                          &any_unknown, &owner, &mixed);
        return xa_return_storage_prepass_finish_branch_summary(any_known, any_unknown, owner, mixed,
                                                               owner_out, mixed_out);
    }
    if (direct && direct->type == AST_MATCH_EXPR) {
        bool any_known = false;
        bool any_unknown = false;
        uint8_t owner = XR_STORAGE_NONE;
        bool mixed = false;
        MatchExprNode *match = &direct->as.match_expr;
        for (int i = 0; i < match->arm_count; i++) {
            xa_return_storage_prepass_accumulate_expr_summary(
                scan, xa_return_storage_prepass_match_arm_value(match->arms[i]), &any_known,
                &any_unknown, &owner, &mixed);
        }
        return xa_return_storage_prepass_finish_branch_summary(any_known, any_unknown, owner, mixed,
                                                               owner_out, mixed_out);
    }
    if (direct && direct->type == AST_MATCH_ARM)
        return xa_return_storage_prepass_expr_summary(
            scan, xa_return_storage_prepass_match_arm_value(direct), owner_out, mixed_out);
    if (direct && direct->type == AST_CALL_EXPR) {
        uint8_t owner = XR_STORAGE_NONE;
        bool mixed = false;
        if (!xa_call_return_storage_summary(scan ? scan->ctx : NULL, direct, &owner, &mixed, NULL))
            return false;
        if (mixed) {
            if (mixed_out)
                *mixed_out = true;
        } else if (owner_out) {
            *owner_out = owner;
        }
        return true;
    }
    return false;
}

static void xa_return_storage_prepass_scan_stmt(XaReturnStoragePrepass *scan, AstNode *stmt);

static void xa_return_storage_prepass_scan_block(XaReturnStoragePrepass *scan, AstNode *block) {
    if (!scan || !block)
        return;
    int saved_count = scan->count;
    if (block->type == AST_BLOCK) {
        BlockNode *body = &block->as.block;
        for (int i = 0; i < body->count; i++)
            xa_return_storage_prepass_scan_stmt(scan, body->statements[i]);
    } else if (block->type == AST_PROGRAM) {
        ProgramNode *body = &block->as.program;
        for (int i = 0; i < body->count; i++)
            xa_return_storage_prepass_scan_stmt(scan, body->statements[i]);
    } else {
        xa_return_storage_prepass_scan_stmt(scan, block);
    }
    scan->count = saved_count;
}

static void xa_return_storage_prepass_scan_stmt(XaReturnStoragePrepass *scan, AstNode *stmt) {
    if (!scan || !stmt)
        return;
    switch (stmt->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL: {
            uint8_t owner = stmt->type == AST_OWNED_DECL    ? XR_STORAGE_OWNED_SYSTEM
                            : stmt->type == AST_SHARED_DECL ? XR_STORAGE_SHARED_SYSTEM
                                                            : XR_STORAGE_NONE;
            xa_return_storage_prepass_bind(scan, stmt->as.var_decl.name, owner);
            return;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &stmt->as.return_stmt;
            uint8_t owner = XR_STORAGE_NONE;
            bool mixed = false;
            if (ret->value_count == 1 &&
                xa_return_storage_prepass_expr_summary(scan, ret->values[0], &owner, &mixed)) {
                if (mixed)
                    scan->mixed = true;
                else
                    xa_return_storage_prepass_record_owner(scan, owner);
            } else if (ret->value_count > 0) {
                xa_return_storage_prepass_record_unknown(scan);
            }
            return;
        }
        case AST_BLOCK:
        case AST_PROGRAM:
            xa_return_storage_prepass_scan_block(scan, stmt);
            return;
        case AST_IF_STMT:
            xa_return_storage_prepass_scan_block(scan, stmt->as.if_stmt.then_branch);
            xa_return_storage_prepass_scan_block(scan, stmt->as.if_stmt.else_branch);
            return;
        case AST_WHILE_STMT:
            xa_return_storage_prepass_scan_block(scan, stmt->as.while_stmt.body);
            return;
        case AST_FOR_STMT: {
            int saved_count = scan->count;
            xa_return_storage_prepass_scan_stmt(scan, stmt->as.for_stmt.initializer);
            xa_return_storage_prepass_scan_block(scan, stmt->as.for_stmt.body);
            scan->count = saved_count;
            return;
        }
        case AST_FOR_IN_STMT:
            xa_return_storage_prepass_scan_block(scan, stmt->as.for_in_stmt.body);
            return;
        case AST_TRY_CATCH:
            xa_return_storage_prepass_scan_block(scan, stmt->as.try_catch.try_body);
            for (int i = 0; i < stmt->as.try_catch.catch_count; i++) {
                XrCatchClause *clause = stmt->as.try_catch.catch_clauses[i];
                if (clause)
                    xa_return_storage_prepass_scan_block(scan, clause->body);
            }
            return;
        case AST_MATCH_EXPR:
            for (int i = 0; i < stmt->as.match_expr.arm_count; i++)
                xa_return_storage_prepass_scan_stmt(scan, stmt->as.match_expr.arms[i]);
            return;
        case AST_MATCH_ARM:
            xa_return_storage_prepass_scan_block(scan, stmt->as.match_arm.body);
            return;
        default:
            return;
    }
}

typedef struct XaReturnFunctionEffectPrepass {
    XaInferContext *ctx;
    XaSymbolLinks *target;
    bool known;
    bool mixed;
    bool unknown;
} XaReturnFunctionEffectPrepass;

static void xa_return_function_effect_record_unknown(XaReturnFunctionEffectPrepass *scan) {
    if (!scan)
        return;
    if (scan->known)
        scan->mixed = true;
    scan->unknown = true;
}

static void xa_return_function_effect_record_param_summary(XaReturnFunctionEffectPrepass *scan,
                                                           XaSymbolLinks *src) {
    if (!scan || !scan->target || !src)
        return;
    if (!xa_param_effect_summary_is_ready(src)) {
        xa_return_function_effect_record_unknown(scan);
        return;
    }
    if (!scan->known) {
        xa_symbol_links_set_return_function_effect_summary(scan->target, src);
        scan->known = true;
        if (scan->unknown)
            scan->mixed = true;
        return;
    }
    if (!xa_return_function_effect_matches_param_summary(scan->target, src))
        scan->mixed = true;
}

static void xa_return_function_effect_record_return_summary(XaReturnFunctionEffectPrepass *scan,
                                                            XaSymbolLinks *src) {
    if (!scan || !scan->target || !src)
        return;
    if (!src->return_fn_effect_scanned || src->return_fn_effect_mixed ||
        !xa_links_return_function_effect_is_ready(src)) {
        xa_return_function_effect_record_unknown(scan);
        return;
    }
    if (!scan->known) {
        xa_symbol_links_copy_return_function_effect_summary(scan->target, src);
        scan->target->return_fn_effect_scan_in_progress = true;
        scan->known = true;
        if (scan->unknown)
            scan->mixed = true;
        return;
    }
    if (!xa_return_function_effect_matches_return_summary(scan->target, src))
        scan->mixed = true;
}

static bool xa_return_function_effect_prepass_record_expr(XaReturnFunctionEffectPrepass *scan,
                                                          AstNode *expr) {
    if (!scan || !expr)
        return false;

    AstNode *block_value = NULL;
    if ((expr->type == AST_BLOCK || expr->type == AST_PROGRAM) &&
        (block_value = xa_lifecycle_lint_single_expr_block_value(expr)))
        return xa_return_function_effect_prepass_record_expr(scan, block_value);

    AstNode *source = xa_direct_function_value_source(expr);
    if (source) {
        XaSymbol *sym = xa_function_value_source_symbol(scan->ctx, source);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(scan->ctx->analyzer, sym) : NULL;
        if (!links) {
            xa_return_function_effect_record_unknown(scan);
            return true;
        }
        xa_return_function_effect_record_param_summary(scan, links);
        return true;
    }

    XaSymbolLinks *callee_links = xa_call_return_function_effect_links(scan->ctx, expr, NULL);
    if (callee_links) {
        xa_return_function_effect_record_return_summary(scan, callee_links);
        return true;
    }

    return false;
}

static void xa_return_function_effect_prepass_scan_stmt(XaReturnFunctionEffectPrepass *scan,
                                                        AstNode *stmt);

static void xa_return_function_effect_prepass_scan_block(XaReturnFunctionEffectPrepass *scan,
                                                         AstNode *block) {
    if (!scan || !block)
        return;
    if (block->type == AST_BLOCK) {
        BlockNode *body = &block->as.block;
        for (int i = 0; i < body->count; i++)
            xa_return_function_effect_prepass_scan_stmt(scan, body->statements[i]);
    } else if (block->type == AST_PROGRAM) {
        ProgramNode *body = &block->as.program;
        for (int i = 0; i < body->count; i++)
            xa_return_function_effect_prepass_scan_stmt(scan, body->statements[i]);
    } else {
        xa_return_function_effect_prepass_scan_stmt(scan, block);
    }
}

static void xa_return_function_effect_prepass_scan_stmt(XaReturnFunctionEffectPrepass *scan,
                                                        AstNode *stmt) {
    if (!scan || !stmt)
        return;
    switch (stmt->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &stmt->as.return_stmt;
            if (ret->value_count != 1 ||
                !xa_return_function_effect_prepass_record_expr(scan, ret->values[0]))
                xa_return_function_effect_record_unknown(scan);
            return;
        }
        case AST_BLOCK:
        case AST_PROGRAM:
            xa_return_function_effect_prepass_scan_block(scan, stmt);
            return;
        case AST_IF_STMT:
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.if_stmt.then_branch);
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.if_stmt.else_branch);
            return;
        case AST_WHILE_STMT:
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.while_stmt.body);
            return;
        case AST_FOR_STMT:
            xa_return_function_effect_prepass_scan_stmt(scan, stmt->as.for_stmt.initializer);
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.for_stmt.body);
            return;
        case AST_FOR_IN_STMT:
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.for_in_stmt.body);
            return;
        case AST_TRY_CATCH:
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.try_catch.try_body);
            for (int i = 0; i < stmt->as.try_catch.catch_count; i++) {
                XrCatchClause *clause = stmt->as.try_catch.catch_clauses[i];
                if (clause)
                    xa_return_function_effect_prepass_scan_block(scan, clause->body);
            }
            return;
        case AST_MATCH_EXPR:
            for (int i = 0; i < stmt->as.match_expr.arm_count; i++)
                xa_return_function_effect_prepass_scan_stmt(scan, stmt->as.match_expr.arms[i]);
            return;
        case AST_MATCH_ARM:
            xa_return_function_effect_prepass_scan_block(scan, stmt->as.match_arm.body);
            return;
        default:
            return;
    }
}

static void xa_ensure_function_return_function_effect_prepass(XaInferContext *ctx,
                                                              XaSymbolLinks *links) {
    if (!ctx || !links || links->return_fn_effect_scan_in_progress)
        return;
    if (links->return_fn_effect_scanned)
        return;

    XrType *return_type = xa_links_function_return_type(links);
    if (!return_type || !XR_TYPE_IS_FUNCTION(return_type)) {
        links->return_fn_effect_scanned = true;
        return;
    }

    AstNode *fn_node = links->function_decl_node;
    if (!fn_node || fn_node->type != AST_FUNCTION_DECL || !fn_node->as.function_decl.body) {
        links->return_fn_effect_scanned = true;
        return;
    }

    links->return_fn_effect_scan_in_progress = true;
    xa_symbol_links_clear_return_function_effect_summary(links);
    XaReturnFunctionEffectPrepass scan = {.ctx = ctx, .target = links};
    xa_return_function_effect_prepass_scan_block(&scan, fn_node->as.function_decl.body);

    if (scan.known && !scan.mixed && !scan.unknown) {
        links->return_fn_effect_mixed = false;
    } else if (scan.mixed || (scan.known && scan.unknown)) {
        xa_symbol_links_clear_return_function_effect_summary(links);
        links->return_fn_effect_mixed = true;
    } else {
        xa_symbol_links_clear_return_function_effect_summary(links);
        links->return_fn_effect_mixed = false;
    }
    links->return_fn_effect_scanned = true;
    links->return_fn_effect_scan_in_progress = false;
}

static void xa_ensure_function_return_storage_prepass(XaInferContext *ctx, XaSymbolLinks *links) {
    if (!ctx || !links || links->return_storage_scan_in_progress)
        return;
    if (links->return_storage_scanned &&
        (links->return_storage_known || links->return_storage_mixed))
        return;
    AstNode *fn_node = links->function_decl_node;
    if (!fn_node || fn_node->type != AST_FUNCTION_DECL || !fn_node->as.function_decl.body)
        return;

    links->return_storage_scan_in_progress = true;
    XaReturnStoragePrepass scan = {.ctx = ctx};
    xa_return_storage_prepass_scan_block(&scan, fn_node->as.function_decl.body);

    if (scan.known && !scan.mixed && !scan.unknown) {
        links->return_storage_owner = scan.owner;
        links->return_storage_known = true;
        links->return_storage_mixed = false;
    } else if (scan.mixed || (scan.known && scan.unknown)) {
        links->return_storage_known = false;
        links->return_storage_mixed = true;
    }
    links->return_storage_scanned = true;
    links->return_storage_scan_in_progress = false;
}

static void xa_record_return_storage_owner(XaInferContext *ctx, uint8_t owner) {
    if (!ctx)
        return;
    if (owner != XR_STORAGE_OWNED_SYSTEM && owner != XR_STORAGE_SHARED_SYSTEM)
        return;
    if (!ctx->return_storage_known) {
        ctx->return_storage_owner = owner;
        ctx->return_storage_known = true;
        if (ctx->return_storage_unknown)
            ctx->return_storage_mixed = true;
        return;
    }
    if (ctx->return_storage_owner != owner)
        ctx->return_storage_mixed = true;
}

static void xa_record_return_storage_unknown(XaInferContext *ctx) {
    if (!ctx)
        return;
    if (ctx->return_storage_known)
        ctx->return_storage_mixed = true;
    ctx->return_storage_unknown = true;
}

static void xa_record_return_storage_mixed(XaInferContext *ctx) {
    if (!ctx)
        return;
    ctx->return_storage_mixed = true;
}

static XaSymbol *xa_lookup_shared_source_symbol(XaInferContext *ctx, AstNode *source) {
    if (!ctx || !source || source->type != AST_VARIABLE || !source->as.variable.name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, source->as.variable.name);
    if (!sym && ctx->analyzer->global_scope)
        sym = xa_scope_lookup(ctx->analyzer->global_scope, source->as.variable.name);
    return sym;
}

static bool xa_type_is_ref_free_owned_freeze_root(XrType *type) {
    if (!type || !XR_TYPE_IS_ARRAY(type) || !type->container.element_type)
        return false;
    XrType *elem = type->container.element_type;
    if (elem->is_nullable)
        return false;
    switch (elem->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static void xa_check_shared_initializer_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                 XrType *init_type) {
    if (!ctx || !decl_node)
        return;
    VarDeclNode *var = &decl_node->as.var_decl;
    if (decl_node->type != AST_SHARED_DECL || !var->initializer)
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(var->initializer, &is_move);
    if (!source)
        return;

    XaSymbol *src_sym = xa_lookup_shared_source_symbol(ctx, source);
    if (!src_sym || (src_sym->kind != XA_SYM_VARIABLE && src_sym->kind != XA_SYM_PARAMETER))
        return;

    XrLocation loc = {
        .file = ctx->file_path, .line = var->initializer->line, .column = var->initializer->column};
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";

    if (src_sym->is_shared) {
        if (!is_move)
            return;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "shared binding '%s' is already a shared identity and must not be moved",
                 src_name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return;
    }

    if (is_move && src_sym->is_owned) {
        if (xa_type_is_ref_free_owned_freeze_root(init_type))
            return;
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "shared binding from owned value '%s' requires a ref-free Array<T> until the "
                 "owned graph freeze verifier is complete; use copy(%s) for an independent "
                 "shared clone",
                 src_name, src_name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return;
    }

    if (!xa_type_needs_borrow_escape_guard(init_type))
        return;

    char msg[256];
    if (is_move) {
        snprintf(msg, sizeof(msg),
                 "move cannot promote local reference value '%s' into a shared binding; "
                 "use copy(%s)",
                 src_name, src_name);
    } else {
        snprintf(msg, sizeof(msg),
                 "shared binding from local reference value '%s' requires copy(%s)", src_name,
                 src_name);
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_owned_initializer_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                XrType *init_type) {
    if (!ctx || !decl_node || decl_node->type != AST_OWNED_DECL)
        return;
    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;
    if (!xa_type_needs_borrow_escape_guard(init_type))
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(var->initializer, &is_move);
    if (!source || is_move)
        return;

    XaSymbol *src_sym = xa_lookup_shared_source_symbol(ctx, source);
    if (!src_sym || src_sym->kind != XA_SYM_VARIABLE)
        return;

    XrLocation loc = {
        .file = ctx->file_path, .line = var->initializer->line, .column = var->initializer->column};
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "owned binding from existing reference value '%s' requires move %s or copy(%s)",
             src_name, src_name, src_name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_const_initializer_alias_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                      XrType *init_type) {
    if (!ctx || !decl_node || decl_node->type != AST_CONST_DECL)
        return;

    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;
    if (!xa_type_needs_borrow_escape_guard(init_type) || xr_type_is_const(init_type))
        return;

    XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, var->initializer);
    if (!root || root->kind != XA_SYM_VARIABLE || root->is_readonly_binding ||
        xa_symbol_has_shared_provenance(root))
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = var->initializer->line ? var->initializer->line : decl_node->line,
        .column = var->initializer->column ? var->initializer->column : decl_node->column,
    };
    const char *name = root->name ? root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg), "const binding from mutable reference value '%s' requires copy(%s)",
             name, name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_const_owned_move_initializer_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                           XrType *init_type) {
    if (!ctx || !decl_node || decl_node->type != AST_CONST_DECL)
        return;

    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;
    if (!xa_type_needs_borrow_escape_guard(init_type))
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(var->initializer, &is_move);
    if (!source || !is_move)
        return;

    XaSymbol *root = xa_lookup_shared_source_symbol(ctx, source);
    if (!root || (root->kind != XA_SYM_VARIABLE && root->kind != XA_SYM_PARAMETER) ||
        !root->is_owned)
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = var->initializer->line ? var->initializer->line : decl_node->line,
        .column = var->initializer->column ? var->initializer->column : decl_node->column,
    };
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";
    const char *dst_name = var->name ? var->name : "?";
    char msg[320];
    snprintf(msg, sizeof(msg),
             "const binding '%s' cannot take moved owned value '%s'; use owned %s = move %s "
             "or copy(%s)",
             dst_name, src_name, dst_name, src_name, src_name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_decl_return_storage_boundary(XaInferContext *ctx, AstNode *decl_node) {
    if (!ctx || !decl_node)
        return;
    if (decl_node->type != AST_VAR_DECL && decl_node->type != AST_CONST_DECL &&
        decl_node->type != AST_OWNED_DECL && decl_node->type != AST_SHARED_DECL)
        return;

    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;

    AstNode *init = xa_storage_boundary_identity_source(var->initializer);
    uint8_t owner = XR_STORAGE_NONE;
    const char *fn_name = NULL;
    if (!xa_call_return_storage_owner(ctx, init, &owner, &fn_name)) {
        const char *value_name = NULL;
        if (xa_call_is_unknown_storage_function_value(ctx, init, &value_name)) {
            XrLocation loc = {
                .file = ctx->file_path,
                .line = var->initializer->line ? var->initializer->line : decl_node->line,
                .column = var->initializer->column ? var->initializer->column : decl_node->column,
            };
            const char *target_kind = decl_node->type == AST_CONST_DECL    ? "const"
                                      : decl_node->type == AST_OWNED_DECL  ? "owned"
                                      : decl_node->type == AST_SHARED_DECL ? "shared"
                                                                           : "var";
            char msg[384];
            snprintf(msg, sizeof(msg),
                     "%s binding '%s' cannot receive storage-sensitive return from function "
                     "value '%s'; call a known function directly or use copy(%s())",
                     target_kind, var->name ? var->name : "?", value_name ? value_name : "?",
                     value_name ? value_name : "?");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
        return;
    }
    if (owner != XR_STORAGE_OWNED_SYSTEM && owner != XR_STORAGE_SHARED_SYSTEM)
        return;

    bool target_owned = decl_node->type == AST_OWNED_DECL;
    bool target_shared = decl_node->type == AST_SHARED_DECL;
    if ((owner == XR_STORAGE_OWNED_SYSTEM && target_owned) ||
        (owner == XR_STORAGE_SHARED_SYSTEM && target_shared))
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = var->initializer->line ? var->initializer->line : decl_node->line,
        .column = var->initializer->column ? var->initializer->column : decl_node->column,
    };
    const char *dst_name = var->name ? var->name : "?";
    const char *target_kind = decl_node->type == AST_CONST_DECL    ? "const"
                              : decl_node->type == AST_OWNED_DECL  ? "owned"
                              : decl_node->type == AST_SHARED_DECL ? "shared"
                                                                   : "var";
    char msg[384];
    if (owner == XR_STORAGE_OWNED_SYSTEM) {
        snprintf(msg, sizeof(msg),
                 "%s binding '%s' cannot receive owned return from function '%s'; use owned %s = "
                 "%s() or copy(%s())",
                 target_kind, dst_name, fn_name ? fn_name : "?", dst_name, fn_name ? fn_name : "?",
                 fn_name ? fn_name : "?");
    } else {
        snprintf(msg, sizeof(msg),
                 "%s binding '%s' cannot receive shared return from function '%s'; use shared %s = "
                 "%s() or copy(%s())",
                 target_kind, dst_name, fn_name ? fn_name : "?", dst_name, fn_name ? fn_name : "?",
                 fn_name ? fn_name : "?");
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_var_owned_alias_initializer_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                          XrType *init_type) {
    if (!ctx || !decl_node || decl_node->type != AST_VAR_DECL)
        return;

    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;
    if (!xa_type_needs_borrow_escape_guard(init_type))
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(var->initializer, &is_move);
    if (!source)
        return;

    XaSymbol *root = xa_lookup_shared_source_symbol(ctx, source);
    if (!root || (root->kind != XA_SYM_VARIABLE && root->kind != XA_SYM_PARAMETER) ||
        !root->is_owned)
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = var->initializer->line ? var->initializer->line : decl_node->line,
        .column = var->initializer->column ? var->initializer->column : decl_node->column,
    };
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";
    const char *dst_name = var->name ? var->name : "?";
    char msg[320];
    if (is_move) {
        snprintf(msg, sizeof(msg),
                 "var binding '%s' cannot take moved owned value '%s'; use owned %s = move %s "
                 "or copy(%s)",
                 dst_name, src_name, dst_name, src_name, src_name);
    } else {
        snprintf(msg, sizeof(msg),
                 "var binding '%s' from owned value '%s' would create a second owning root; "
                 "use owned %s = move %s or copy(%s)",
                 dst_name, src_name, dst_name, src_name, src_name);
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_assignment_owned_alias_boundary(XaInferContext *ctx, AstNode *node,
                                                     XrType *value_type) {
    if (!ctx || !node || node->type != AST_ASSIGNMENT)
        return;

    AssignmentNode *assign = &node->as.assignment;
    if (!assign->value || xa_call_is_copy_builtin(assign->value))
        return;
    if (!xa_type_needs_borrow_escape_guard(value_type))
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(assign->value, &is_move);
    if (!source)
        return;

    XaSymbol *root = xa_lookup_shared_source_symbol(ctx, source);
    if (!root || (root->kind != XA_SYM_VARIABLE && root->kind != XA_SYM_PARAMETER) ||
        !root->is_owned)
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = assign->value->line ? assign->value->line : node->line,
        .column = assign->value->column ? assign->value->column : node->column,
    };
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";
    const char *dst_name = assign->name ? assign->name : "?";
    char msg[320];
    if (is_move) {
        snprintf(msg, sizeof(msg),
                 "assignment to var '%s' cannot take moved owned value '%s'; use an owned "
                 "binding or copy(%s)",
                 dst_name, src_name, src_name);
    } else {
        snprintf(msg, sizeof(msg),
                 "assignment to var '%s' from owned value '%s' would create a second owning "
                 "root; use copy(%s)",
                 dst_name, src_name, src_name);
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

/* ============================================================================
 * Pass 2: Statement Visitors
 * ============================================================================
 * Type inference for statements: variable declarations, assignments,
 * control flow (if/while/for), and return statements.
 * ========================================================================== */

void xa_visit_var_decl_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    VarDeclNode *var = &node->as.var_decl;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!sym) {
        // Symbol not found (Pass 1 missed this declaration, e.g. inside for/while/if)
        // Define it now so type inference can proceed
        xa_visit_collect_var_decl(ctx, node);
        sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (!sym)
            return;
    }

    /* Ensure symbol_id is set (covers late-discovered declarations). */
    if (var->symbol_id == 0)
        var->symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->const_initializer = sym->is_const ? var->initializer : NULL;
        links->has_ct_value = false;
        links->ct_value = (XrCtValue) {0};
        links->is_comptime_local = ctx->comptime_block_depth > 0;
    }
    if (xa_freestanding_profile_enabled(ctx->analyzer) && node->type == AST_SHARED_DECL &&
        !xa_freestanding_shared_static_initializer_allowed(ctx, var, true)) {
        xa_freestanding_report_unavailable(
            ctx, node, "shared declaration",
            "only int/float/bool/char/string/null consteval initializers or recursively scalar "
            "struct/union consteval initializers are supported as static shared storage in the "
            "current freestanding slice");
    } else if (xa_freestanding_profile_enabled(ctx->analyzer) &&
               xa_is_module_level_scope(ctx->analyzer) && node->type != AST_SHARED_DECL &&
               !(node->type == AST_CONST_DECL && xa_freestanding_top_const_allowed(ctx, var)) &&
               !(node->type == AST_VAR_DECL &&
                 xa_freestanding_top_var_static_initializer_allowed(
                     ctx, var, links ? links->declared_type : NULL))) {
        xa_freestanding_report_unavailable(
            ctx, node,
            node->type == AST_CONST_DECL ? "top-level const declaration"
                                         : "top-level var declaration",
            node->type == AST_CONST_DECL
                ? "only int/float/bool/char/string/null consteval scalars, static string "
                  "fixed-array lanes, and recursively scalar fixed-array/tuple/struct "
                  "initializers are allowed as erased or static data objects in the current "
                  "freestanding slice"
                : "only int/float/bool/char/string/null consteval initializers, typed "
                  "int/float/bool zero defaults, typed nullable scalar/string/null defaults, or "
                  "recursively scalar struct/union consteval initializers are supported as static "
                  "mutable module storage in the current freestanding slice");
    }

    if (node->type == AST_OWNED_DECL && xa_is_module_level_scope(ctx->analyzer)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   "module owned declaration is not supported", &loc);
    }

    // Variable declarations must have a type annotation or initializer.
    if (!var->initializer && !links->declared_type) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' must have a type annotation or initializer",
                 var->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
                                   msg, &loc);
    }

    // Infer type from initializer if no declared type
    XrType *var_type = NULL;
    if (var->initializer) {
        // Set expected_type for bidirectional inference
        XrType *saved_expected = ctx->expected_type;
        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            ctx->expected_type = links->declared_type;
        }
        uint32_t saved_initializing_symbol_id = ctx->initializing_symbol_id;
        ctx->initializing_symbol_id = sym->id;
        XrType *init_type = xa_visit_infer_expr(ctx, var->initializer);
        ctx->initializing_symbol_id = saved_initializing_symbol_id;
        ctx->expected_type = saved_expected;

        // Store inferred initializer type in the analyzer side table
        // (the canonical source for downstream codegen / LSP).
        xa_analyzer_set_node_type(ctx->analyzer, var->initializer, init_type);
        xa_propagate_function_value_summary(ctx, sym, var->initializer, init_type);
        xa_check_shared_initializer_boundary(ctx, node, init_type);
        xa_check_owned_initializer_boundary(ctx, node, init_type);
        xa_check_const_initializer_alias_boundary(ctx, node, init_type);
        xa_check_const_owned_move_initializer_boundary(ctx, node, init_type);
        xa_check_var_owned_alias_initializer_boundary(ctx, node, init_type);
        xa_check_decl_return_storage_boundary(ctx, node);

        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            // Check null safety first (null→T, T?→T)
            bool null_err = xa_check_null_safety(ctx->analyzer, links->declared_type, init_type,
                                                 "Variable initializer", &loc);
            // Check assignment compatibility
            bool type_mismatch = false;
            if (XR_TYPE_IS_FUNCTION(links->declared_type)) {
                type_mismatch = xa_function_assignment_mismatch(links->declared_type, init_type);
            } else {
                type_mismatch = !xa_typecheck_assignable(links->declared_type, init_type);
            }
            if (!null_err && type_mismatch) {
                // Json→concrete type: allowed at compile time, runtime check inserted by
                // codegen. e.g. var x: int = data["key"] is legal but requires runtime validation.
                if (XR_TYPE_IS_FUNCTION(links->declared_type) ||
                    !xr_is_json_coercion(links->declared_type, init_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Type '%s' is not assignable to type '%s'",
                             xr_type_to_string(init_type), xr_type_to_string(links->declared_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            var_type = links->declared_type;
        } else {
            var_type = init_type;
            // Empty array literal without type annotation: require explicit type
            if (var->initializer && var->initializer->type == AST_ARRAY_LITERAL &&
                var->initializer->as.array_literal.count == 0 && XR_TYPE_IS_ARRAY(init_type) &&
                init_type->container.element_type &&
                XR_TYPE_IS_UNKNOWN(init_type->container.element_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                    "Empty array '[]' requires a type annotation, e.g. var x: Array<int> = []",
                    &loc);
            }
        }
    } else if (links->declared_type) {
        var_type = links->declared_type;
        /* Reject non-default-initializable types without explicit initializer.
         * e.g. `var u: User` is a compile error, but `var x: int` is allowed. */
        if (!XR_TYPE_IS_UNKNOWN(links->declared_type) &&
            !xa_type_is_default_initializable(ctx, links->declared_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Type '%s' is not default-initializable; "
                     "variable '%s' requires an explicit initializer",
                     xr_type_to_string(links->declared_type), var->name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    } else {
        // Fallback for missing type (error already reported above)
        var_type = xr_type_new_unknown(NULL);
    }

    bool shared_provenance = var->initializer
                                 ? xa_expr_yields_shared_provenance(ctx, var->initializer, var_type)
                                 : false;

    if (sym->is_readonly_binding && !sym->is_rebindable && var_type)
        var_type = xr_type_make_const(ctx->analyzer->isolate, var_type);
    if (!sym->is_const && !sym->is_shared && !sym->is_owned && sym->is_rebindable)
        sym->is_shared_provenance = shared_provenance;

    if (var->initializer && xa_is_module_level_scope(ctx->analyzer) &&
        xa_type_contains_span_view(var_type)) {
        const char *context = node->type == AST_SHARED_DECL
                                  ? "store Slice view in shared binding"
                                  : "store Slice view in module-level binding";
        xa_check_span_value_escape(ctx, var->initializer, var_type, context);
    }
    if (var->initializer &&
        (xa_is_module_level_scope(ctx->analyzer) || node->type == AST_SHARED_DECL) && var_type &&
        XR_TYPE_IS_POINTER(var_type)) {
        const char *context = node->type == AST_SHARED_DECL
                                  ? "store raw pointer borrow in shared binding"
                                  : "store raw pointer borrow in module-level binding";
        xa_check_pointer_borrow_escape(ctx, var->initializer, var->initializer, var_type, context);
    }

    char freestanding_var_context[160];
    snprintf(freestanding_var_context, sizeof(freestanding_var_context), "variable '%s'",
             var->name ? var->name : "?");
    xa_freestanding_report_tagged_type_unavailable(ctx, node, var_type, freestanding_var_context);

    links->type = var_type;
    xa_update_borrowed_alias_root(ctx, sym, var->initializer, var_type);
    xa_register_active_span_borrow(ctx, sym, var->initializer, var_type);
    xa_record_pointer_provenance(ctx, sym, var->initializer, var_type);

    if (var_type && xa_type_is_concurrency_handle(var_type)) {
        if (!sym->is_shared) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            const char *label = xa_concurrency_handle_label(var_type);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s handle must be declared with 'shared'",
                     label ? label : "synchronization");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    }

    // Track definite assignment
    // Variables with type annotations are initialized to the type zero value.
    links->is_definitely_assigned = (var->initializer != NULL) || (links->declared_type != NULL);

    // A const with a proven compile-time initializer, or any comptime-block
    // local binding, can be reused by later consteval expressions.
    if ((sym->is_const || links->is_comptime_local) && var->initializer) {
        XrCtValue value = {0};
        const char *err = NULL;
        if (xa_consteval_expr(ctx->analyzer, var->initializer, &value, &err)) {
            links->has_ct_value = true;
            links->ct_value = value;
            links->is_const_foldable = true;
        } else if (links->is_comptime_local) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(
                msg, sizeof(msg),
                "comptime block local binding initializer must be evaluable at compile time%s%s",
                err ? ": " : "", err ? err : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    } else if (links->is_comptime_local && !var->initializer) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   "comptime block local binding requires an initializer", &loc);
    }
    xa_validate_static_data_attrs(ctx, node, var, links, var_type);
    links->assign_count = var->initializer ? 1 : 0;

    // Detect loop variable context
    XaScope *s = ctx->analyzer->current_scope;
    while (s) {
        if (s->kind == XA_SCOPE_LOOP) {
            links->is_loop_variable = true;
            break;
        }
        s = s->parent;
    }

    // Store the inferred type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, var_type);

    // Create assignment flow node
    if (ctx->flow) {
        xa_flow_create_assignment(ctx->flow, NULL, var->name, var_type);
    }
}

void xa_visit_assignment_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    AssignmentNode *assign = &node->as.assignment;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, assign->name);

    if (!sym) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Undeclared variable '%s'", assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                   msg, &loc);
        return;
    }

    /* Write back resolved symbol ID for Xi lowering (Braun SSA key). */
    assign->symbol_id = sym->id;
    xa_parallel_capture_check(ctx, node, sym, true);

    // Record write reference for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (assign->name ? strlen(assign->name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, true);  // is_write=true
    }

    // Check immutable binding assignment
    if (sym->is_const || sym->is_shared || sym->is_owned || !sym->is_rebindable) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        const char *fmt = sym->is_shared  ? "Cannot assign to shared binding '%s'"
                          : sym->is_owned ? "Cannot assign to owned binding '%s'"
                                          : "Cannot assign to const '%s'";
        snprintf(msg, sizeof(msg), fmt, assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    // Check in-parameter immutability: cannot reassign an 'in' parameter
    if (sym->kind == XA_SYM_PARAMETER && sym->passing_mode == XR_PARAM_IN) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot assign to 'in' parameter '%s' (readonly reference)",
                 assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    XrType *var_type = xa_analyzer_get_type(ctx->analyzer, sym);
    if (xa_freestanding_profile_enabled(ctx->analyzer) && sym->scope &&
        sym->scope->kind == XA_SCOPE_GLOBAL && xa_type_has_fixed_layout_data_object(var_type)) {
        if (!assign->value || assign->value->type != AST_STRUCT_LITERAL) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                "freestanding profile only supports struct-literal whole-value assignment to "
                "static aggregate top-level var",
                &loc);
            return;
        }
    }
    xa_check_active_span_borrow_owner_mutation(ctx, node, sym, "reassigning the owner");

    // Bidirectional inference: propagate target type to value expression
    XrType *saved_expected = ctx->expected_type;
    if (var_type && !XR_TYPE_IS_UNKNOWN(var_type)) {
        ctx->expected_type = var_type;
    }
    XrType *value_type = xa_visit_infer_expr(ctx, assign->value);
    ctx->expected_type = saved_expected;
    xa_check_assignment_owned_alias_boundary(ctx, node, value_type);
    if ((sym->is_shared || (sym->scope && sym->scope->kind == XA_SCOPE_GLOBAL)) && value_type &&
        XR_TYPE_IS_POINTER(value_type)) {
        const char *context = sym->is_shared ? "store raw pointer borrow in shared binding"
                                             : "store raw pointer borrow in module-level binding";
        xa_check_pointer_borrow_escape(ctx, node, assign->value, value_type, context);
    }
    if (!sym->is_const && !sym->is_shared && !sym->is_owned && sym->is_rebindable)
        sym->is_shared_provenance =
            xa_expr_yields_shared_provenance(ctx, assign->value, value_type);

    // Mark as definitely assigned.
    if (links) {
        links->is_definitely_assigned = true;
        links->assign_count++;
    }
    xa_update_borrowed_alias_root(ctx, sym, assign->value, value_type);
    xa_register_active_span_borrow(ctx, sym, assign->value, value_type);
    xa_record_pointer_provenance(ctx, sym, assign->value, value_type);

    xa_assign_check_type(ctx, node, var_type, value_type, assign->name, NULL);

    // Update flow graph — but only if value type is known.
    // Recording unknown would downgrade a variable from its declared type.
    if (ctx->flow && value_type && !XR_TYPE_IS_UNKNOWN(value_type)) {
        xa_flow_create_assignment(ctx->flow, NULL, assign->name, value_type);
    }
}

struct XaOutParamDaState {
    XaSymbolLinks *links;
    bool before_assigned;
    bool then_assigned;
    bool else_assigned;
    bool path_merge_has_fallthrough;
    bool path_merge_assigned;
    bool path_merge_fields_initialized;
    XaOutFieldDaPath *before_fields;
    XaOutFieldDaPath *then_fields;
    XaOutFieldDaPath *else_fields;
    XaOutFieldDaPath *path_merge_fields;
};

XR_FUNC XaOutParamDaState *xa_out_param_da_capture(XaInferContext *ctx, int *out_count) {
    if (out_count)
        *out_count = 0;
    XaScope *function_scope = xa_find_enclosing_function_scope(ctx);
    if (!ctx || !ctx->analyzer || !function_scope || !out_count)
        return NULL;

    int symbol_count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(function_scope, &symbol_count);
    if (!symbols || symbol_count <= 0) {
        xr_free(symbols);
        return NULL;
    }

    XaOutParamDaState *states = xr_calloc((size_t) symbol_count + 1u, sizeof(XaOutParamDaState));
    if (!states) {
        xr_free(symbols);
        return NULL;
    }

    int count = 0;
    for (int i = 0; i < symbol_count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_PARAMETER || sym->passing_mode != XR_PARAM_OUT)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
        if (!links)
            continue;
        states[count].links = links;
        states[count].before_assigned = links->is_definitely_assigned;
        states[count].then_assigned = links->is_definitely_assigned;
        states[count].else_assigned = links->is_definitely_assigned;
        states[count].path_merge_has_fallthrough = false;
        states[count].path_merge_assigned = true;
        states[count].path_merge_fields_initialized = false;
        states[count].before_fields = xa_symbol_links_clone_out_field_da_paths(links);
        states[count].then_fields = xa_symbol_links_clone_out_field_da_paths(links);
        states[count].else_fields = xa_symbol_links_clone_out_field_da_paths(links);
        states[count].path_merge_fields = NULL;
        count++;
    }
    xr_free(symbols);

    if (count <= 0) {
        xr_free(states);
        return NULL;
    }
    *out_count = count;
    return states;
}

XR_FUNC void xa_out_param_da_restore_before(XaOutParamDaState *states, int count) {
    for (int i = 0; states && i < count; i++) {
        if (states[i].links) {
            states[i].links->is_definitely_assigned = states[i].before_assigned;
            xa_symbol_links_restore_out_field_da_paths(states[i].links, states[i].before_fields);
        }
    }
}

XR_FUNC void xa_out_param_da_begin_path_merge(XaOutParamDaState *states, int count) {
    for (int i = 0; states && i < count; i++) {
        states[i].path_merge_has_fallthrough = false;
        states[i].path_merge_assigned = true;
        states[i].path_merge_fields_initialized = false;
        xa_symbol_links_free_out_field_da_paths(states[i].path_merge_fields);
        states[i].path_merge_fields = NULL;
    }
}

XR_FUNC void xa_out_param_da_record_path(XaOutParamDaState *states, int count, bool falls_through) {
    if (!falls_through)
        return;
    for (int i = 0; states && i < count; i++) {
        states[i].path_merge_has_fallthrough = true;
        states[i].path_merge_assigned =
            states[i].path_merge_assigned &&
            (states[i].links ? states[i].links->is_definitely_assigned : states[i].before_assigned);
        XaOutFieldDaPath *path_fields =
            states[i].links ? xa_symbol_links_clone_out_field_da_paths(states[i].links) : NULL;
        if (!states[i].path_merge_fields_initialized) {
            states[i].path_merge_fields = path_fields;
            states[i].path_merge_fields_initialized = true;
        } else {
            XaOutFieldDaPath *merged = xa_symbol_links_intersect_out_field_da_paths(
                states[i].path_merge_fields, path_fields);
            xa_symbol_links_free_out_field_da_paths(states[i].path_merge_fields);
            xa_symbol_links_free_out_field_da_paths(path_fields);
            states[i].path_merge_fields = merged;
        }
    }
}

XR_FUNC void xa_out_param_da_apply_path_merge(XaOutParamDaState *states, int count) {
    for (int i = 0; states && i < count; i++) {
        if (states[i].links && states[i].path_merge_has_fallthrough) {
            states[i].links->is_definitely_assigned = states[i].path_merge_assigned;
            xa_symbol_links_restore_out_field_da_paths(states[i].links,
                                                       states[i].path_merge_fields);
        }
    }
}

XR_FUNC void xa_out_param_da_free(XaOutParamDaState *states) {
    for (int i = 0; states && states[i].links; i++) {
        xa_symbol_links_free_out_field_da_paths(states[i].before_fields);
        xa_symbol_links_free_out_field_da_paths(states[i].then_fields);
        xa_symbol_links_free_out_field_da_paths(states[i].else_fields);
        xa_symbol_links_free_out_field_da_paths(states[i].path_merge_fields);
    }
    xr_free(states);
}

static void xa_out_param_da_record_then(XaOutParamDaState *states, int count) {
    for (int i = 0; states && i < count; i++) {
        states[i].then_assigned =
            states[i].links ? states[i].links->is_definitely_assigned : states[i].before_assigned;
        xa_symbol_links_free_out_field_da_paths(states[i].then_fields);
        states[i].then_fields =
            states[i].links ? xa_symbol_links_clone_out_field_da_paths(states[i].links) : NULL;
    }
}

static void xa_out_param_da_record_else(XaOutParamDaState *states, int count) {
    for (int i = 0; states && i < count; i++) {
        states[i].else_assigned =
            states[i].links ? states[i].links->is_definitely_assigned : states[i].before_assigned;
        xa_symbol_links_free_out_field_da_paths(states[i].else_fields);
        states[i].else_fields =
            states[i].links ? xa_symbol_links_clone_out_field_da_paths(states[i].links) : NULL;
    }
}

static void xa_out_param_da_apply_if_merge(XaOutParamDaState *states, int count,
                                           bool then_falls_through, bool else_falls_through) {
    for (int i = 0; states && i < count; i++) {
        if (!states[i].links)
            continue;
        bool merged = states[i].before_assigned;
        if (then_falls_through && else_falls_through) {
            merged = states[i].then_assigned && states[i].else_assigned;
        } else if (then_falls_through) {
            merged = states[i].then_assigned;
        } else if (else_falls_through) {
            merged = states[i].else_assigned;
        }
        states[i].links->is_definitely_assigned = merged;
        XaOutFieldDaPath *merged_fields = NULL;
        if (then_falls_through && else_falls_through) {
            merged_fields = xa_symbol_links_intersect_out_field_da_paths(states[i].then_fields,
                                                                         states[i].else_fields);
        } else if (then_falls_through) {
            merged_fields = states[i].then_fields;
        } else if (else_falls_through) {
            merged_fields = states[i].else_fields;
        } else {
            merged_fields = states[i].before_fields;
        }
        xa_symbol_links_restore_out_field_da_paths(states[i].links, merged_fields);
    }
}

void xa_visit_if_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    IfStmtNode *if_stmt = &node->as.if_stmt;

    // Analyze condition
    XrType *cond_type = xa_visit_infer_expr(ctx, if_stmt->condition);
    xa_check_condition_type(ctx, if_stmt->condition, cond_type);

    // Flow graph handles all type narrowing via TRUE_CONDITION / FALSE_CONDITION
    // nodes. apply_condition_narrowing() in xanalyzer_flow.c recognizes patterns:
    //   x != null, typeof(x) == Type.xxx, x is Type, truthiness, &&, ||
    // Early-return narrowing is automatic: when then-branch terminates,
    // its flow becomes unreachable → merge label only has the false-condition
    // path → opposite narrowing applies to subsequent code.

    int out_da_count = 0;
    XaOutParamDaState *out_da = xa_out_param_da_capture(ctx, &out_da_count);
    bool then_falls_through = xa_statement_can_fall_through(if_stmt->then_branch);
    bool else_falls_through =
        if_stmt->else_branch ? xa_statement_can_fall_through(if_stmt->else_branch) : true;

    XaFlowNode *saved = ctx->flow ? ctx->flow->current_flow : NULL;

    // Then branch: flow enters TRUE_CONDITION
    if (ctx->flow) {
        ctx->flow->current_flow = xa_flow_create_condition(ctx->flow, if_stmt->condition, true);
    }
    xa_visit_infer_stmt(ctx, if_stmt->then_branch);
    xa_out_param_da_record_then(out_da, out_da_count);
    XaFlowNode *then_end = ctx->flow ? ctx->flow->current_flow : NULL;

    // Else branch: flow enters FALSE_CONDITION
    if (ctx->flow)
        ctx->flow->current_flow = saved;
    xa_out_param_da_restore_before(out_da, out_da_count);

    XaFlowNode *else_end = NULL;
    if (if_stmt->else_branch) {
        if (ctx->flow) {
            ctx->flow->current_flow =
                xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
        }
        xa_visit_infer_stmt(ctx, if_stmt->else_branch);
        else_end = ctx->flow ? ctx->flow->current_flow : NULL;
    }
    xa_out_param_da_record_else(out_da, out_da_count);
    xa_out_param_da_apply_if_merge(out_da, out_da_count, then_falls_through, else_falls_through);
    xa_out_param_da_free(out_da);

    // Merge branches
    if (ctx->flow) {
        XaFlowNode *merge = xa_flow_create_branch_label(ctx->flow);
        if (then_end)
            xa_flow_add_antecedent(merge, then_end);
        if (else_end) {
            xa_flow_add_antecedent(merge, else_end);
        } else {
            // No else: false-condition path flows through to merge
            ctx->flow->current_flow = saved;
            XaFlowNode *false_cond = xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
            xa_flow_add_antecedent(merge, false_cond);
        }
        ctx->flow->current_flow = xa_flow_finish_label(ctx->flow, merge);
    }
}

void xa_visit_while_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    WhileStmtNode *while_stmt = &node->as.while_stmt;

    // Create loop label
    XaFlowNode *loop_start = NULL;
    if (ctx->flow) {
        loop_start = xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    XrType *cond_type = xa_visit_infer_expr(ctx, while_stmt->condition);
    xa_check_condition_type(ctx, while_stmt->condition, cond_type);

    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, true);
    }

    int out_da_count = 0;
    XaOutParamDaState *out_da = xa_out_param_da_capture(ctx, &out_da_count);

    /* Analyze body. A block body goes through xa_visit_block_stmt so it
     * gets its own scope keyed on the body node, matching Pass 1. */
    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, while_stmt->label, node);
    if (while_stmt->body)
        xa_visit_infer_stmt(ctx, while_stmt->body);
    xa_loop_scope_pop(ctx, &loop_scope);

    // Back edge to loop start
    if (ctx->flow && loop_start) {
        xa_flow_add_antecedent(loop_start, ctx->flow->current_flow);
    }
    xa_out_param_da_restore_before(out_da, out_da_count);
    xa_out_param_da_free(out_da);

    // Exit condition
    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, false);
    }
}

void xa_visit_for_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ForStmtNode *for_stmt = &node->as.for_stmt;

    // Enter loop scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    // Analyze initializer
    if (for_stmt->initializer) {
        xa_visit_infer_stmt(ctx, for_stmt->initializer);
    }

    // Create loop label
    if (ctx->flow) {
        xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    if (for_stmt->condition) {
        XrType *cond_type = xa_visit_infer_expr(ctx, for_stmt->condition);
        xa_check_condition_type(ctx, for_stmt->condition, cond_type);
    }

    int out_da_count = 0;
    XaOutParamDaState *out_da = xa_out_param_da_capture(ctx, &out_da_count);

    // Analyze body - inline block to match Pass 1 scope structure
    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, for_stmt->label, node);
    if (for_stmt->body)
        xa_visit_inline_statement_sequence_with_cursor(ctx, for_stmt->body);
    xa_loop_scope_pop(ctx, &loop_scope);

    // Analyze increment
    if (for_stmt->increment) {
        xa_visit_infer_stmt(ctx, for_stmt->increment);
    }
    xa_out_param_da_restore_before(out_da, out_da_count);
    xa_out_param_da_free(out_da);

    xa_clear_active_span_borrows_in_scope(ctx, ctx->analyzer->current_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
}

static XaScope *xa_find_enclosing_function_scope(XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    XaScope *scope = ctx->analyzer->current_scope;
    while (scope && scope->kind != XA_SCOPE_FUNCTION)
        scope = scope->parent;
    return scope;
}

XR_FUNC bool xa_statement_can_fall_through(AstNode *node) {
    if (!node)
        return true;

    switch (node->type) {
        case AST_RETURN_STMT:
        case AST_THROW_STMT:
            return false;
        case AST_BLOCK:
        case AST_PROGRAM: {
            AstNode **statements = NULL;
            int count = 0;
            if (!xa_block_node_statements(node, &statements, &count) || count <= 0)
                return true;
            return xa_statement_can_fall_through(statements[count - 1]);
        }
        case AST_IF_STMT:
            return !node->as.if_stmt.else_branch ||
                   xa_statement_can_fall_through(node->as.if_stmt.then_branch) ||
                   xa_statement_can_fall_through(node->as.if_stmt.else_branch);
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (xa_statement_can_fall_through(tc->try_body))
                return true;
            for (int i = 0; i < tc->catch_count; i++) {
                XrCatchClause *cc = tc->catch_clauses ? tc->catch_clauses[i] : NULL;
                if (!cc || xa_statement_can_fall_through(cc->body))
                    return true;
            }
            return false;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            if (!m->arms || m->arm_count <= 0)
                return true;
            for (int i = 0; i < m->arm_count; i++) {
                AstNode *arm = m->arms[i];
                if (!arm || arm->type != AST_MATCH_ARM ||
                    xa_statement_can_fall_through(arm->as.match_arm.body))
                    return true;
            }
            return false;
        }
        default:
            return true;
    }
}

static void xa_report_unassigned_out_params(XaInferContext *ctx, XaScope *function_scope,
                                            AstNode *loc_node, const char *exit_label) {
    if (!ctx || !ctx->analyzer || !function_scope || !loc_node)
        return;

    int symbol_count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(function_scope, &symbol_count);
    for (int i = 0; i < symbol_count; i++) {
        XaSymbol *sym = symbols ? symbols[i] : NULL;
        if (!sym || sym->kind != XA_SYM_PARAMETER || sym->passing_mode != XR_PARAM_OUT)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
        if (!links || links->is_definitely_assigned)
            continue;

        XrLocation loc = {
            .file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "out parameter '%s' must be assigned before %s",
                 sym->name ? sym->name : "?", exit_label ? exit_label : "function exits");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_USED_BEFORE_ASSIGN, msg, &loc);
    }
    xr_free(symbols);
}

static void xa_check_out_params_assigned_before_return(XaInferContext *ctx, XaScope *function_scope,
                                                       AstNode *return_node) {
    xa_report_unassigned_out_params(ctx, function_scope, return_node, "returning");
}

XR_FUNC void xa_check_out_params_assigned_at_function_exit(XaInferContext *ctx,
                                                           XaScope *function_scope,
                                                           AstNode *body_node) {
    if (!ctx || !ctx->analyzer || !function_scope || !body_node)
        return;
    if (!xa_statement_can_fall_through(body_node))
        return;
    xa_report_unassigned_out_params(ctx, function_scope, body_node, "function exits");
}

void xa_visit_return_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ReturnStmtNode *ret = &node->as.return_stmt;

    /* Top-level return is illegal — must be inside a function body. */
    XaScope *function_scope = xa_find_enclosing_function_scope(ctx);
    if (!function_scope) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_CMP_INVALID_RETURN,
                                   "'return' outside of a function body", &loc);
        return;
    }

    XrType *return_type = xr_type_new_unit(NULL);

    if (ret->value_count == 0) {
        // No return value
        return_type = xr_type_new_unit(NULL);
    } else if (ret->value_count == 1) {
        // Single return value
        if (ret->values[0]) {
            // Bidirectional inference: propagate declared return type to return expr
            // (e.g., return (x) => x + 1 inside fn(): fn(int): int)
            XrType *saved_expected = ctx->expected_type;
            if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
                ctx->expected_type = ctx->expected_return_type;
            } else {
                // Look up enclosing function's declared return type from scope
                XaScope *s = ctx->analyzer->current_scope;
                while (s && s->kind != XA_SCOPE_FUNCTION)
                    s = s->parent;
                if (s && s->ast_node) {
                    AstNode *fn_node = (AstNode *) s->ast_node;
                    XrType *decl_ret = NULL;
                    if (fn_node->type == AST_FUNCTION_DECL &&
                        fn_node->as.function_decl.return_type) {
                        decl_ret = xr_tref_resolve(ctx->analyzer->isolate,
                                                   fn_node->as.function_decl.return_type);
                    } else if (fn_node->type == AST_METHOD_DECL &&
                               fn_node->as.method_decl.return_type) {
                        decl_ret = xr_tref_resolve(ctx->analyzer->isolate,
                                                   fn_node->as.method_decl.return_type);
                    }
                    if (decl_ret && !XR_TYPE_IS_UNKNOWN(decl_ret)) {
                        ctx->expected_type = decl_ret;
                    }
                }
            }
            return_type = xa_visit_infer_expr(ctx, ret->values[0]);
            ctx->expected_type = saved_expected;
            xa_check_borrowed_return_escape(ctx, node, ret->values[0], return_type);
            xa_check_span_return_escape(ctx, node, return_type);
            xa_check_pointer_borrow_escape(ctx, node, ret->values[0], return_type,
                                           "return raw pointer borrow");
            bool is_move = false;
            AstNode *source = xa_shared_boundary_source(ret->values[0], &is_move);
            XaSymbol *root = source ? xa_lookup_shared_source_symbol(ctx, source) : NULL;
            if (root && (root->kind == XA_SYM_VARIABLE || root->kind == XA_SYM_PARAMETER) &&
                root->is_owned && !is_move) {
                XrLocation loc = {.file = ctx->file_path,
                                  .line = ret->values[0]->line ? ret->values[0]->line : node->line,
                                  .column = ret->values[0]->column ? ret->values[0]->column
                                                                   : node->column};
                const char *name = source->as.variable.name ? source->as.variable.name : "?";
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "returning owned value '%s' requires move %s or copy(%s)", name, name,
                         name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            } else if (root && (root->kind == XA_SYM_VARIABLE || root->kind == XA_SYM_PARAMETER) &&
                       root->is_owned && is_move) {
                xa_record_return_storage_owner(ctx, XR_STORAGE_OWNED_SYSTEM);
            } else if (root && root->kind == XA_SYM_VARIABLE &&
                       xa_symbol_has_shared_provenance(root)) {
                xa_record_return_storage_owner(ctx, XR_STORAGE_SHARED_SYSTEM);
            } else {
                AstNode *ret_expr = xa_storage_boundary_identity_source(ret->values[0]);
                uint8_t owner = XR_STORAGE_NONE;
                bool mixed = false;
                XaReturnStoragePrepass ret_scan = {.ctx = ctx, .use_scope_lookup = true};
                if (xa_return_storage_prepass_expr_summary(&ret_scan, ret_expr, &owner, &mixed)) {
                    if (mixed)
                        xa_record_return_storage_mixed(ctx);
                    else
                        xa_record_return_storage_owner(ctx, owner);
                } else if (xa_call_is_unknown_storage_function_value(ctx, ret_expr, NULL)) {
                    XrLocation loc = {
                        .file = ctx->file_path,
                        .line = ret->values[0]->line ? ret->values[0]->line : node->line,
                        .column = ret->values[0]->column ? ret->values[0]->column : node->column};
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                        "return from function value has unknown storage; call a known function "
                        "directly or return copy(f())",
                        &loc);
                    xa_record_return_storage_mixed(ctx);
                } else {
                    xa_record_return_storage_unknown(ctx);
                }
            }
        }
    } else {
        // Legacy AST multi-expression return is treated as a tuple type.
        XrType **element_types = xr_malloc(sizeof(XrType *) * ret->value_count);
        for (int i = 0; i < ret->value_count; i++) {
            if (ret->values[i]) {
                element_types[i] = xa_visit_infer_expr(ctx, ret->values[i]);
                xa_check_borrowed_return_escape(ctx, node, ret->values[i], element_types[i]);
                xa_check_pointer_borrow_escape(ctx, node, ret->values[i], element_types[i],
                                               "return raw pointer borrow");
            } else {
                element_types[i] = xr_type_new_unknown(NULL);
            }
        }
        // Create tuple type for the legacy AST shape.
        return_type = xr_type_new_tuple(ctx->analyzer->isolate, element_types, ret->value_count);

        // Store return type info in the analyzer side table.
        xa_analyzer_set_node_type(ctx->analyzer, node, return_type);
        xa_check_span_return_escape(ctx, node, return_type);

        xr_free(element_types);
    }

    // Collect return type for function inference
    xa_infer_add_return_type(ctx, return_type);
    xa_freestanding_report_tagged_type_unavailable(ctx, node, return_type, "return value");

    // Check against expected return type (strict: Unit and concrete types enforced)
    if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
        if (!xa_typecheck_assignable(ctx->expected_return_type, return_type)) {
            // Json→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
            if (!xr_is_json_coercion(ctx->expected_return_type, return_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg), "Return type mismatch: expected '%s', got '%s'",
                         xr_type_to_string(ctx->expected_return_type),
                         xr_type_to_string(return_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
    }

    xa_check_out_params_assigned_before_return(ctx, function_scope, node);

    // Mark flow as unreachable after return
    if (ctx->flow) {
        ctx->flow->current_flow = ctx->flow->unreachable_flow;
    }
}

void xa_visit_block_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    xa_visit_inline_statement_sequence_with_cursor(ctx, node);

    xa_clear_active_span_borrows_in_scope(ctx, ctx->analyzer->current_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
}
