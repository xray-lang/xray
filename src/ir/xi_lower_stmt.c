/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_stmt.c - Compound statement lowering (extracted from xi_lower.c)
 *
 * Contains: select, scope_block, for-in loops, try-catch, match expressions.
 * These are the larger, self-contained statement/expression lowering functions.
 */

#include "xi_lower_internal.h"
#include "xi.h"
#include "xi_effect.h"
#include "xi_own.h"
#include "xi_lower_expr_helpers.h"
#include "../analysis/xglobal_summary.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/class/xclass_info.h"
#include "../base/xchecks.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/parser/xtype_ref.h"

#include <string.h>
#include <stdio.h>

/* Forward declaration */
static void lower_stmts(XiLower *l, AstNode **stmts, int count);

static void xi_lower_record_global_asm(XiLower *l, AstNode *node) {
    if (!l || !node || node->type != AST_GLOBAL_ASM)
        return;
    if (l->global_asm_count >= 65535) {
        l->had_error = true;
        return;
    }
    if (l->global_asm_count >= l->global_asm_cap) {
        int new_cap = l->global_asm_cap == 0 ? 4 : l->global_asm_cap * 2;
        l->global_asm_templates = (const char **) xr_realloc(
            l->global_asm_templates, (size_t) new_cap * sizeof(const char *));
        XR_CHECK(l->global_asm_templates != NULL, "xi_lower: global asm vector OOM");
        l->global_asm_cap = new_cap;
    }
    l->global_asm_templates[l->global_asm_count++] =
        node->as.global_asm.text ? node->as.global_asm.text : "";
}

static bool lower_is_comptime_block_expr(AstNode *node) {
    return node && node->type == AST_COMPTIME_EXPR && node->as.comptime_expr.expr &&
           node->as.comptime_expr.expr->type == AST_BLOCK;
}

static int xi_lower_sync_runtime_class_global_index(const char *name) {
    if (!name)
        return -1;
    if (strcmp(name, "WorkQueue") == 0)
        return XR_GLOBAL_VAR_WORKQUEUE;
    if (strcmp(name, "ResultGroup") == 0)
        return XR_GLOBAL_VAR_RESULTGROUP;
    if (strcmp(name, "CountdownLatch") == 0)
        return XR_GLOBAL_VAR_COUNTDOWNLATCH;
    if (strcmp(name, "Semaphore") == 0)
        return XR_GLOBAL_VAR_SEMAPHORE;
    if (strcmp(name, "EventCount") == 0)
        return XR_GLOBAL_VAR_EVENTCOUNT;
    return -1;
}

static XiValue *xi_lower_emit_sync_runtime_builtin_class(XiLower *l, const char *name, int line) {
    int index = xi_lower_sync_runtime_class_global_index(name);
    if (index < 0)
        return NULL;
    struct XrType *type = xr_type_new_class(NULL, name);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, type, 0);
    if (v) {
        v->aux_int = index;
        v->aux = (void *) arena_strdup(l->func, name);
        v->line = (uint32_t) line;
    }
    return v;
}

XiValue *xi_lower_bool_condition(XiLower *l, XiValue *cond) {
    if (!cond || !cond->type)
        return cond;
    if (cond->type->kind == XR_KIND_BOOL && !cond->type->is_nullable)
        return cond;
    if (cond->type->is_nullable) {
        XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
        if (!is_null)
            return cond;
        is_null->args[0] = cond;
        XiValue *not_null = xi_value_new(l->func, l->cur_block, XI_NOT, l->type_bool, 1);
        if (!not_null)
            return is_null;
        not_null->args[0] = is_null;
        return not_null;
    }
    return cond;
}

static XiValue *lower_guard_expr(XiLower *l, AstNode *guard_node) {
    XiValue *guard = xi_lower_expr(l, guard_node);
    return guard ? xi_lower_bool_condition(l, guard) : NULL;
}

static void xi_lower_loop_push(XiLower *l, XiLoopTarget *target, const char *label,
                               XiBlock *break_target, XiBlock *continue_target) {
    XR_DCHECK(l != NULL, "xi_lower_loop_push: NULL lowerer");
    XR_DCHECK(target != NULL, "xi_lower_loop_push: NULL target");
    target->label = label;
    target->break_target = break_target;
    target->continue_target = continue_target;
    target->cleanup_scope_depth = l->cleanup_scope_depth;
    target->prev = l->loop_targets;
    l->loop_targets = target;
    l->break_target = break_target;
    l->continue_target = continue_target;
}

static void xi_lower_loop_pop(XiLower *l, XiLoopTarget *target) {
    XR_DCHECK(l != NULL, "xi_lower_loop_pop: NULL lowerer");
    XR_DCHECK(target != NULL, "xi_lower_loop_pop: NULL target");
    l->loop_targets = target->prev;
    l->break_target = target->prev ? target->prev->break_target : NULL;
    l->continue_target = target->prev ? target->prev->continue_target : NULL;
}

static XiLoopTarget *xi_lower_loop_find(XiLower *l, const char *label) {
    if (!l)
        return NULL;
    if (!label)
        return l->loop_targets;
    for (XiLoopTarget *it = l->loop_targets; it; it = it->prev) {
        if (it->label && strcmp(it->label, label) == 0)
            return it;
    }
    return NULL;
}

static void stmt_set_missing_line(XiValue *v, int line) {
    if (v && v->line == 0 && line > 0)
        v->line = (uint32_t) line;
}

static int stmt_json_field_index(struct XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !type->object.field_names || !name)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (!type->object.field_names[i])
            return -1;
    }
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static uint16_t stmt_narrow_op_for_type(struct XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->scalar_rep == XR_NATIVE_I64)
        return 0;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
            return XI_NARROW_I8;
        case XR_NATIVE_U8:
            return XI_NARROW_U8;
        case XR_NATIVE_I16:
            return XI_NARROW_I16;
        case XR_NATIVE_U16:
            return XI_NARROW_U16;
        case XR_NATIVE_I32:
            return XI_NARROW_I32;
        case XR_NATIVE_U32:
            return XI_NARROW_U32;
        default:
            return 0;
    }
}

static bool stmt_type_is_unsigned_int(struct XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static int stmt_print_slot_hint_for_value(XiValue *v) {
    if (!v || !v->type)
        return 0;
    // Nullable primitives must print through the tagged path so a `null`
    // value renders as "null" rather than its raw numeric payload (0).
    if (v->type->is_nullable)
        return 0;
    if (v->type->kind == XR_KIND_FLOAT)
        return 2;
    if (stmt_type_is_unsigned_int(v->type))
        return 3;
    if (v->type->kind == XR_KIND_INT)
        return 1;
    return 0;
}

static bool lower_cleanup_discard_empty_panic_interval(XiLower *l, XiCleanupScope *scope) {
    if (!l || !scope || !scope->active_try || !scope->active_try->block || !l->cur_block)
        return false;
    XiBlock *try_block = scope->active_try->block;
    if (try_block->succs[0] != l->cur_block || try_block->succs[1] || l->cur_block->nvalues != 0 ||
        try_block->nvalues == 0 || try_block->values[try_block->nvalues - 1] != scope->active_try)
        return false;
    if (scope->panic_edge_count == 0 ||
        scope->panic_edges[scope->panic_edge_count - 1].try_op != scope->active_try)
        return false;

    /* No instruction can panic between registration and consumption. Keep
     * the static cleanup ladder, but erase the empty handler interval so a
     * loop with a tail-position cleanup does not execute setjmp per iteration. */
    try_block->nvalues--;
    scope->panic_edge_count--;
    scope->active_try = NULL;
    return true;
}

static void lower_cleanup_emit_end_try(XiLower *l, XiCleanupScope *scope, int line) {
    if (!l || !scope || !l->cur_block || !scope->active_try)
        return;
    if (lower_cleanup_discard_empty_panic_interval(l, scope))
        return;
    XiValue *end = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
    if (!end)
        return;
    end->aux = (void *) scope->active_try;
    end->flags |= XI_FLAG_SIDE_EFFECT;
    end->line = (uint32_t) line;
}

static void lower_cleanup_frontier(XiLower *l, XiCleanupScope *scope, uint16_t count) {
    if (!l || !scope || !l->cur_block)
        return;
    if (count > scope->site_count)
        count = scope->site_count;
    for (uint16_t i = count; i > 0 && l->cur_block; i--) {
        XiCleanupSite *site = &scope->sites[i - 1];
        XiValue *enter = xi_value_new(l->func, l->cur_block, XI_CLEANUP_ENTER, l->type_unit, 0);
        if (!enter) {
            l->had_error = true;
            return;
        }
        enter->flags |= XI_FLAG_SIDE_EFFECT;
        enter->line = site->line;
        int saved_try_base = l->cleanup_body_try_base_depth;
        if (l->cleanup_body_depth == 0)
            l->cleanup_body_try_base_depth = l->try_depth;
        l->cleanup_body_depth++;
        if (site->kind == XI_CLEANUP_SITE_BLOCK && site->statement)
            xi_lower_stmt(l, site->statement->as.defer_stmt.body);
        else if (site->kind == XI_CLEANUP_SITE_PARALLEL_PLAN_END && site->value)
            (void) xi_lower_parallel_plan_lifecycle_call(l, NULL, site->value, "_end");
        l->cleanup_body_depth--;
        l->cleanup_body_try_base_depth = saved_try_base;
        if (l->cur_block) {
            XiValue *leave = xi_value_new(l->func, l->cur_block, XI_CLEANUP_LEAVE, l->type_unit, 0);
            if (!leave) {
                l->had_error = true;
                return;
            }
            leave->flags |= XI_FLAG_SIDE_EFFECT;
            leave->line = site->line;
        }
    }
}

static void lower_cleanup_compile_panic_edges(XiLower *l, XiCleanupScope *scope) {
    if (!l || !scope)
        return;
    XiBlock *saved_block = l->cur_block;
    bool saved_dead_after_throw = l->dead_after_throw;
    for (uint16_t i = 0; i < scope->panic_edge_count; i++) {
        XiCleanupPanicEdge *edge = &scope->panic_edges[i];
        if (!edge->block || !edge->try_op)
            continue;
        if (edge->block->npreds == 0 && edge->try_op->block)
            xi_block_add_pred(edge->block, edge->try_op->block);
        xi_lower_braun_seal(l, edge->block);
        l->cur_block = edge->block;
        l->dead_after_throw = false;

        XiValue *caught =
            xi_value_new(l->func, l->cur_block, XI_CATCH,
                         xi_lower_type_or_any(l, NULL, "cleanup panic propagation",
                                              edge->try_op ? (int) edge->try_op->line : 0),
                         0);
        if (!caught)
            continue;
        caught->aux = (void *) edge->try_op;
        caught->flags |= XI_FLAG_SIDE_EFFECT;
        caught->line = edge->try_op->line;

        lower_cleanup_frontier(l, scope, edge->frontier_count);
        if (l->cur_block) {
            XiValue *rethrow = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_unit, 1);
            if (rethrow) {
                rethrow->args[0] = caught;
                rethrow->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
                rethrow->line = caught->line;
            }
            l->cur_block->kind = XI_BLOCK_UNREACHABLE;
            l->cur_block->control = caught;
            l->cur_block = NULL;
        }
    }
    l->cur_block = saved_block;
    l->dead_after_throw = saved_dead_after_throw;
}

XR_FUNC void xi_lower_cleanup_scope_push(XiLower *l) {
    XR_DCHECK(l != NULL, "xi_lower_cleanup_scope_push: NULL lowerer");
    XR_CHECK(l->cleanup_scope_depth < XI_MAX_CLEANUP_SCOPE_NESTING,
             "cleanup scope nesting limit exceeded");
    memset(&l->cleanup_scopes[l->cleanup_scope_depth], 0, sizeof(XiCleanupScope));
    l->cleanup_scope_depth++;
}

XR_FUNC void xi_lower_cleanup_scope_pop_normal(XiLower *l, int line) {
    XR_DCHECK(l != NULL, "xi_lower_cleanup_scope_pop_normal: NULL lowerer");
    if (l->cleanup_scope_depth <= 0)
        return;
    XiCleanupScope *scope = &l->cleanup_scopes[l->cleanup_scope_depth - 1];
    if (l->cur_block && scope->active_try) {
        lower_cleanup_emit_end_try(l, scope, line);
        lower_cleanup_frontier(l, scope, scope->site_count);
    }
    lower_cleanup_compile_panic_edges(l, scope);
    xr_free(scope->sites);
    xr_free(scope->panic_edges);
    memset(scope, 0, sizeof(*scope));
    l->cleanup_scope_depth--;
}

XR_FUNC void xi_lower_cleanup_run_to_depth(XiLower *l, int target_depth, int line) {
    XR_DCHECK(l != NULL, "xi_lower_cleanup_run_to_depth: NULL lowerer");
    if (!l->cur_block)
        return;
    if (target_depth < 0)
        target_depth = 0;
    if (target_depth > l->cleanup_scope_depth)
        target_depth = l->cleanup_scope_depth;
    for (int i = l->cleanup_scope_depth - 1; i >= target_depth && l->cur_block; i--) {
        XiCleanupScope *scope = &l->cleanup_scopes[i];
        if (!scope->active_try)
            continue;
        lower_cleanup_emit_end_try(l, scope, line);
        lower_cleanup_frontier(l, scope, scope->site_count);
    }
}

XR_FUNC bool xi_lower_cleanup_has_active_site(XiLower *l) {
    if (!l)
        return false;
    for (int i = l->cleanup_scope_depth - 1; i >= 0; i--) {
        if (l->cleanup_scopes[i].active_try)
            return true;
    }
    return false;
}

static XiCleanupSite *lower_cleanup_scope_append_site(XiCleanupScope *scope) {
    if (!scope || scope->site_count == UINT16_MAX)
        return NULL;
    if (scope->site_count >= scope->site_capacity) {
        uint16_t capacity = scope->site_capacity ? (uint16_t) (scope->site_capacity * 2u) : 4u;
        if (capacity < scope->site_capacity)
            capacity = UINT16_MAX;
        XiCleanupSite *grown =
            (XiCleanupSite *) xr_realloc(scope->sites, (size_t) capacity * sizeof(XiCleanupSite));
        if (!grown)
            return NULL;
        scope->sites = grown;
        scope->site_capacity = capacity;
    }
    XiCleanupSite *site = &scope->sites[scope->site_count++];
    memset(site, 0, sizeof(*site));
    return site;
}

static bool lower_cleanup_scope_append_block(XiCleanupScope *scope, AstNode *statement) {
    XiCleanupSite *site = lower_cleanup_scope_append_site(scope);
    if (!site || !statement)
        return false;
    site->statement = statement;
    site->kind = XI_CLEANUP_SITE_BLOCK;
    site->line = (uint32_t) statement->line;
    return true;
}

static bool lower_cleanup_scope_append_panic_edge(XiCleanupScope *scope, XiBlock *block,
                                                  XiValue *try_op) {
    if (!scope || !block || !try_op || scope->panic_edge_count == UINT16_MAX)
        return false;
    if (scope->panic_edge_count >= scope->panic_edge_capacity) {
        uint16_t capacity =
            scope->panic_edge_capacity ? (uint16_t) (scope->panic_edge_capacity * 2u) : 4u;
        if (capacity < scope->panic_edge_capacity)
            capacity = UINT16_MAX;
        XiCleanupPanicEdge *grown = (XiCleanupPanicEdge *) xr_realloc(
            scope->panic_edges, (size_t) capacity * sizeof(XiCleanupPanicEdge));
        if (!grown)
            return false;
        scope->panic_edges = grown;
        scope->panic_edge_capacity = capacity;
    }
    XiCleanupPanicEdge *edge = &scope->panic_edges[scope->panic_edge_count++];
    edge->block = block;
    edge->try_op = try_op;
    edge->frontier_count = scope->site_count;
    return true;
}

static bool lower_cleanup_open_panic_interval(XiLower *l, XiCleanupScope *scope, int line) {
    if (!l || !scope || !l->cur_block)
        return false;
    XiBlock *registration_block = l->cur_block;
    XiBlock *panic_block = xi_block_new(l->func);
    XiBlock *continuation = xi_block_new(l->func);
    XiValue *try_op = xi_value_new(l->func, registration_block, XI_TRY, l->type_unit, 0);
    if (!panic_block || !continuation || !try_op)
        return false;
    try_op->aux = (void *) panic_block;
    try_op->aux_int = XI_TRY_AUX_STATIC_CLEANUP;
    try_op->flags |= XI_FLAG_SIDE_EFFECT;
    try_op->line = (uint32_t) line;
    /* End the registration block at the exact program point covered by this
     * hidden panic edge. The verifier and ownership passes can then seed the
     * cold cleanup block from the pre-body state instead of from normal-path
     * releases that occur later in the same source block. */
    xi_block_set_jump(registration_block, continuation);
    xi_lower_braun_seal(l, continuation);
    l->cur_block = continuation;
    scope->active_try = try_op;
    return lower_cleanup_scope_append_panic_edge(scope, panic_block, try_op);
}

XR_FUNC bool xi_lower_cleanup_register_parallel_end(XiLower *l, AstNode *node, XiValue *plan) {
    if (!l || !l->cur_block || !plan)
        return false;
    if (l->cleanup_scope_depth <= 0)
        xi_lower_cleanup_scope_push(l);
    XiCleanupScope *scope = &l->cleanup_scopes[l->cleanup_scope_depth - 1];
    int line = node ? node->line : 0;
    if (scope->active_try)
        lower_cleanup_emit_end_try(l, scope, line);
    XiCleanupSite *site = lower_cleanup_scope_append_site(scope);
    if (!site)
        return false;
    site->kind = XI_CLEANUP_SITE_PARALLEL_PLAN_END;
    site->value = plan;
    site->line = (uint32_t) line;
    return lower_cleanup_open_panic_interval(l, scope, line);
}

static bool stmt_value_is_fresh_value_struct(XiValue *v) {
    if (!v || xi_var_id_is_valid(v->var_id))
        return false;
    return v->op == XI_AGG_NEW || v->op == XI_FIXED_ARRAY_NEW || v->op == XI_FIXED_BYTES_CONST ||
           (v->op == XI_COPY && v->aux_int == XI_COPY_KIND_VALUE_CLONE);
}

static void stmt_mark_value_clone_copy(XiValue *v) {
    if (v && v->op == XI_COPY)
        v->aux_int = XI_COPY_KIND_VALUE_CLONE;
}

static XiValue *stmt_narrow_for_target_type(XiLower *l, AstNode *node, XiValue *val,
                                            struct XrType *target_type) {
    if (!val || !val->type || !target_type)
        return val;
    if (XR_TYPE_IS_FLOAT(val->type) && XR_TYPE_IS_FLOAT(target_type)) {
        if (xr_type_equals(target_type, val->type))
            return val;
        if (target_type->scalar_rep == XR_NATIVE_F32) {
            XiValue *n = xi_value_new(l->func, l->cur_block, XI_NARROW_F32, target_type, 1);
            if (!n)
                return val;
            n->args[0] = val;
            n->line = (uint32_t) node->line;
            return n;
        }
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
        if (!copy)
            return val;
        copy->args[0] = val;
        copy->line = (uint32_t) node->line;
        return copy;
    }
    if (!XR_TYPE_IS_INT(val->type))
        return val;
    uint16_t narrow_op = stmt_narrow_op_for_type(target_type);
    if (!narrow_op) {
        if (target_type && XR_TYPE_IS_INT(target_type) && !xr_type_equals(target_type, val->type)) {
            XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
            if (!copy)
                return val;
            copy->args[0] = val;
            copy->line = (uint32_t) node->line;
            return copy;
        }
        return val;
    }
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, target_type, 1);
    if (!n)
        return val;
    n->args[0] = val;
    n->line = (uint32_t) node->line;
    return n;
}

static XaSymbol *stmt_lookup_class_symbol(XiLower *l, const char *name) {
    if (!l || !l->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(l->analyzer, name);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_in_scope(l->analyzer, name, l->analyzer->global_scope);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_deep(l->analyzer, name);
    return (sym && sym->kind == XA_SYM_CLASS) ? sym : NULL;
}

static XaSymbol *stmt_lookup_enum_symbol(XiLower *l, const char *name) {
    if (!l || !l->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(l->analyzer, name);
    if (sym && sym->kind == XA_SYM_ENUM)
        return sym;
    sym = xa_analyzer_lookup_in_scope(l->analyzer, name, l->analyzer->global_scope);
    if (sym && sym->kind == XA_SYM_ENUM)
        return sym;
    sym = xa_analyzer_lookup_deep(l->analyzer, name);
    return (sym && sym->kind == XA_SYM_ENUM) ? sym : NULL;
}

static const char *stmt_adt_subject_enum_name(struct XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ENUM) {
        if (type->enum_type.enum_name)
            return type->enum_type.enum_name;
        return type->instance.class_name;
    }
    if ((type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
        type->instance.class_name)
        return type->instance.class_name;
    return NULL;
}

static bool stmt_adt_variant_names(const AstNode *variant, const char **owner_out,
                                   const char **member_out) {
    if (owner_out)
        *owner_out = NULL;
    if (member_out)
        *member_out = NULL;
    if (!variant)
        return false;
    if (variant->type == AST_ENUM_ACCESS) {
        if (owner_out)
            *owner_out = variant->as.enum_access.enum_name;
        if (member_out)
            *member_out = variant->as.enum_access.member_name;
        return variant->as.enum_access.member_name != NULL;
    }
    if (variant->type == AST_MEMBER_ACCESS) {
        const MemberAccessNode *ma = &variant->as.member_access;
        if (member_out)
            *member_out = ma->name;
        if (owner_out && ma->object && ma->object->type == AST_VARIABLE)
            *owner_out = ma->object->as.variable.name;
        return ma->name != NULL;
    }
    return false;
}

static int stmt_adt_member_index(XiLower *l, struct XrType *subject_type, const AstNode *variant) {
    const char *owner_name = NULL;
    const char *member_name = NULL;
    if (!stmt_adt_variant_names(variant, &owner_name, &member_name) || !member_name)
        return -1;

    const char *subject_name = stmt_adt_subject_enum_name(subject_type);
    const char *enum_name = subject_name ? subject_name : owner_name;
    if (!enum_name)
        return -1;
    if (subject_name && owner_name && strcmp(subject_name, owner_name) != 0)
        return -1;

    XaSymbol *enum_sym = stmt_lookup_enum_symbol(l, enum_name);
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, enum_sym);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (!info)
        return -1;
    return xa_enum_info_find_variant(info, member_name);
}

static XaSymbolLinks *stmt_adt_subject_links(XiLower *l, struct XrType *subject_type) {
    const char *enum_name = stmt_adt_subject_enum_name(subject_type);
    if (!enum_name)
        return NULL;
    XaSymbol *enum_sym = stmt_lookup_enum_symbol(l, enum_name);
    return xa_analyzer_get_links(l->analyzer, enum_sym);
}

static XrAggregateLayout *stmt_lookup_struct_layout(XiLower *l, const char *name) {
    XaSymbol *sym = stmt_lookup_class_symbol(l, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return (links && links->class_info) ? links->class_info->struct_layout : NULL;
}

static XrAggregateLayout *stmt_type_struct_layout(XiLower *l, struct XrType *type) {
    XrAggregateLayout *layout = xi_lower_struct_layout_of(type);
    if (layout)
        return layout;
    const char *class_name = xr_type_get_class_name(type);
    return class_name ? stmt_lookup_struct_layout(l, class_name) : NULL;
}

static bool stmt_type_needs_value_clone(XiLower *l, struct XrType *type) {
    if (!type)
        return false;
    return type->kind == XR_KIND_FIXED_ARRAY || type->is_value_type ||
           stmt_type_struct_layout(l, type) != NULL;
}

/* Keep large mutable value structs in one addressable local for their whole
 * source lifetime.  Braun SSA is excellent for scalar values, but a mutating
 * method on a large aggregate otherwise requires a whole-value PLACE_LOAD
 * writeback after every call.  A stable place preserves the same value
 * semantics while allowing both VM and native backends to update the original
 * storage directly.  Small aggregates remain in SSA so ordinary scalar
 * replacement and register passing stay cheap. */
#define XI_STABLE_VALUE_LOCAL_MIN_BYTES 32u

static bool stmt_function_may_suspend(XiLower *l) {
    if (!l || !l->func)
        return true;
    /* Storage placement asks whether control can leave this body and come back,
     * so both suspension kinds count: a generator frame outlives `yield expr`
     * exactly as a coroutine frame outlives `await`. */
    if (((l->func->semantic_effects | l->func->unknown_semantic_effects) &
         XA_SEM_EFFECT_ANY_SUSPEND) != 0)
        return true;
    if (!l->global_evidence || l->func->xg_body_func_id == XG_NO_ID)
        return false;
    const XgBodySummary *body = NULL;
    for (uint32_t i = 0; i < l->global_evidence->nbodies; i++) {
        if (l->global_evidence->bodies[i].func_id == l->func->xg_body_func_id) {
            body = &l->global_evidence->bodies[i];
            break;
        }
    }
    if (!body)
        return true;
    if ((body->capability_bits & XG_CAP_COROUTINE) != 0)
        return true;
    uint32_t effects = 0;
    return xg_body_effects_compose_closed_world_calls(l->global_evidence, body, &effects) &&
           (effects & XG_BODY_MAY_SUSPEND) != 0;
}

static bool stmt_decl_prefers_stable_value_place(XiLower *l, AstNode *node, int var_id,
                                                 struct XrType *type) {
    if (!l || !l->analyzer || !node || var_id < 0 || var_id >= l->var_count)
        return false;
    if (xi_lower_cleanup_symbol_needs_place(l, node->as.var_decl.symbol_id))
        return !(l->is_program && l->shared_map && l->shared_map[var_id] >= 0);
    if (node->type != AST_VAR_DECL || l->vars[var_id].captured_by_child)
        return false;
    /* Coroutine locals that survive suspension must ultimately live in the
     * heap frame.  XI_LOCAL_ADDR currently denotes ordinary function-local
     * storage, so keep suspendable bodies on the SSA path until the coroutine
     * planner can publish a frame-relative stable place. */
    if (stmt_function_may_suspend(l))
        return false;
    if (l->is_program && l->shared_map && l->shared_map[var_id] >= 0)
        return false;
    XaSymbol *symbol =
        node->as.var_decl.symbol_id
            ? xa_scope_lookup_by_id(l->analyzer->global_scope, node->as.var_decl.symbol_id)
            : NULL;
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(l->analyzer, symbol) : NULL;
    if (!links || (!links->value_mutated && links->assign_count <= 1))
        return false;
    XrAggregateLayout *layout = stmt_type_struct_layout(l, type);
    return layout && layout->total_size >= XI_STABLE_VALUE_LOCAL_MIN_BYTES;
}

static bool stmt_bind_stable_value_place(XiLower *l, AstNode *node, int var_id, XiValue *init_val) {
    if (!stmt_decl_prefers_stable_value_place(l, node, var_id, l->vars[var_id].type))
        return true;
    if (xi_lower_cleanup_symbol_needs_place(l, node->as.var_decl.symbol_id))
        return xi_lower_cleanup_bind_place(l, var_id, init_val, node->line);
    XiValue *place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR, l->vars[var_id].type, 1);
    if (!place)
        return false;
    place->args[0] = init_val;
    place->line = (uint32_t) node->line;
    l->vars[var_id].call_place = place;
    l->vars[var_id].place_mode = XR_PARAM_REF;
    return true;
}

static XrClassInfo *stmt_class_info_for_type(XiLower *l, struct XrType *type) {
    if (!l || !type)
        return NULL;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_ref) {
        return type->instance.class_ref;
    }
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;
    XaSymbol *sym = stmt_lookup_class_symbol(l, type->instance.class_name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links ? links->class_info : NULL;
}

static XiValue *stmt_load_class_value(XiLower *l, const char *class_name) {
    if (!l || !class_name)
        return NULL;

    int class_var = xi_lower_var_find(l, 0, class_name);
    if (class_var >= 0) {
        if (l->is_program && l->shared_map[class_var] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[class_var];
            b.name = l->vars[class_var].name;
            b.type = l->vars[class_var].type;
            return xi_lower_emit_top_load(l, b, l->type_any);
        }
        return xi_lower_braun_read(l, class_var, l->cur_block);
    }

    XiTopBinding tb = xi_lower_find_top_binding(l, 0, class_name);
    if (xi_top_binding_valid(tb))
        return xi_lower_emit_top_load(l, tb, l->type_any);

    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, 0, class_name, &upval_type);
    if (upval_idx >= 0) {
        XiValue *cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
        if (cls)
            cls->aux_int = upval_idx;
        return cls;
    }
    return NULL;
}

static XiValue *stmt_default_struct_value_depth(XiLower *l, struct XrType *type, int line,
                                                int depth) {
    if (!l || !type || type->is_nullable)
        return NULL;
    if (depth > 16)
        return NULL;
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;

    XrAggregateLayout *layout = xi_lower_struct_layout_of(type);
    const char *class_name = type->instance.class_name;
    if (!layout && class_name)
        layout = stmt_lookup_struct_layout(l, class_name);
    if (!layout || !class_name)
        return NULL;

    XiValue *cls = stmt_load_class_value(l, class_name);
    if (!cls)
        return NULL;

    XiValue *inst = xi_value_new(l->func, l->cur_block, XI_AGG_NEW, type, 1);
    if (!inst)
        return NULL;
    inst->args[0] = cls;
    inst->aux = (void *) layout;
    inst->flags |= XI_FLAG_SIDE_EFFECT;
    inst->line = (uint32_t) line;

    XrClassInfo *info = stmt_class_info_for_type(l, type);
    if (info) {
        int field_count =
            info->field_count < layout->field_count ? info->field_count : layout->field_count;
        for (int i = 0; i < field_count; i++) {
            if (layout->fields[i].native_type != XR_NATIVE_NESTED_AGGREGATE)
                continue;
            XaSymbol *field = info->fields[i];
            XaSymbolLinks *links = field ? xa_analyzer_get_links(l->analyzer, field) : NULL;
            struct XrType *field_type = links ? links->type : NULL;
            XiValue *nested = stmt_default_struct_value_depth(l, field_type, line, depth + 1);
            if (!nested)
                continue;
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_AGG_SET, l->type_unit, 2);
            if (!set)
                continue;
            set->args[0] = inst;
            set->args[1] = nested;
            set->aux = (void *) layout;
            set->aux_int = i;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) line;
        }
    }
    return inst;
}

static XiValue *stmt_default_struct_value(XiLower *l, struct XrType *type, int line) {
    return stmt_default_struct_value_depth(l, type, line, 0);
}

/* ========== Select Statement ========== */

static XiValue *lower_select_time_after(XiLower *l, SelectCaseNode *sc, int line) {
    XiValue *timeout = xi_lower_expr(l, sc->value);
    if (!timeout || !l->cur_block)
        return NULL;

    struct XrType *timer_type = xr_type_new_channel(l->isolate, l->type_int);
    if (!timer_type)
        timer_type = l->type_any;

    XiValue *timer = xi_value_new(l->func, l->cur_block, XI_TIME_AFTER, timer_type, 1);
    if (!timer)
        return NULL;
    timer->args[0] = timeout;
    timer->line = (uint32_t) line;
    return timer;
}

static struct XrType *stmt_channel_element_type(struct XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_CHANNEL)
        return type->container.element_type;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            struct XrType *elem = stmt_channel_element_type(type->union_type.members[i]);
            if (elem)
                return elem;
        }
    }
    return NULL;
}

static bool stmt_type_is_channel(struct XrType *type) {
    return type && (type->kind == XR_KIND_CHANNEL || stmt_channel_element_type(type) != NULL);
}

static XiValue *lower_chan_recv_status(XiLower *l, XiValue *recv) {
    if (!recv)
        return NULL;
    XiValue *status = xi_value_new(l->func, l->cur_block, XI_CHAN_RECV_STATUS, l->type_bool, 1);
    if (!status)
        return NULL;
    status->args[0] = recv;
    status->line = recv->line;
    return status;
}

static void lower_select_recv_ready_branch(XiLower *l, XiValue *recv_status, XiValue *chan,
                                           XiBlock *body_blk, XiBlock *next_blk) {
    XiValue *is_closed = xi_value_new(l->func, l->cur_block, XI_CHAN_IS_CLOSED, l->type_bool, 1);
    if (is_closed)
        is_closed->args[0] = chan;
    if (!recv_status || !is_closed)
        return;

    XiValue *ready = xi_binary(l->func, l->cur_block, XI_BOR, l->type_bool, recv_status, is_closed);
    if (ready)
        xi_block_set_if(l->cur_block, ready, body_blk, next_blk);
}

/* Emit the explicit dispose of the `after` timer channel at the select merge.
 * The timer value dominates merge; this performs the drop the compiler omits
 * across the select.block suspend. See design/885. */
static void lower_select_emit_timer_dispose(XiLower *l, XiValue *timer_chan_val, int line) {
    if (!timer_chan_val || !l->cur_block)
        return;
    XiValue *dispose = xi_value_new(l->func, l->cur_block, XI_CHAN_TIMER_DISPOSE, l->type_unit, 1);
    if (dispose) {
        dispose->args[0] = timer_chan_val;
        dispose->flags |= XI_FLAG_SIDE_EFFECT;
        dispose->line = (uint32_t) line;
    }
}

#define XI_LOWER_SELECT_STACK_CAP 32
#define XI_LOWER_MAX_SELECT_CASES ((int) UINT16_MAX)

typedef struct LowerSelectLists {
    XiValue **case_channels;
    XiValue **case_send_values;
    uint8_t *case_send_modes;
    XiValue **block_channels;
    int block_channel_count;
    int cap;
} LowerSelectLists;

static bool lower_select_validate_case_count(XiLower *l, int case_count, int line) {
    if (case_count < 0 || case_count > XI_LOWER_MAX_SELECT_CASES) {
        fprintf(stderr, "[LOWER] select case count exceeds %d at line %d\n",
                XI_LOWER_MAX_SELECT_CASES, line);
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_select_lists_init(XiLower *l, LowerSelectLists *lists, int case_count,
                                    XiValue **stack_case_channels, XiValue **stack_case_send_values,
                                    uint8_t *stack_case_send_modes,
                                    XiValue **stack_block_channels) {
    int cap = case_count > 0 ? case_count : 1;
    lists->block_channel_count = 0;
    lists->cap = cap;
    if (cap <= XI_LOWER_SELECT_STACK_CAP) {
        lists->case_channels = stack_case_channels;
        lists->case_send_values = stack_case_send_values;
        lists->case_send_modes = stack_case_send_modes;
        lists->block_channels = stack_block_channels;
        memset(lists->case_send_modes, 0, (size_t) cap * sizeof(uint8_t));
        return true;
    }

    lists->case_channels =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    lists->case_send_values =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    lists->case_send_modes =
        (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(uint8_t)));
    lists->block_channels =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    if (!lists->case_channels || !lists->case_send_values || !lists->case_send_modes ||
        !lists->block_channels) {
        l->had_error = true;
        return false;
    }
    memset(lists->case_channels, 0, (size_t) cap * sizeof(XiValue *));
    memset(lists->case_send_values, 0, (size_t) cap * sizeof(XiValue *));
    memset(lists->case_send_modes, 0, (size_t) cap * sizeof(uint8_t));
    return true;
}

static bool lower_select_block_channel_add(XiLower *l, LowerSelectLists *lists, XiValue *chan,
                                           int line) {
    if (lists->block_channel_count >= lists->cap) {
        fprintf(stderr, "[LOWER] select block channel count exceeds %d at line %d\n", lists->cap,
                line);
        l->had_error = true;
        return false;
    }
    lists->block_channels[lists->block_channel_count++] = chan;
    return true;
}

/* After the case loop: park the select (SELECT_BLOCK over the collected
 * channels, or YIELD for a send-only select with no timeout) and wire the
 * back-edge to try_head, or fall through to merge for a default select. */
static void lower_select_park(XiLower *l, bool has_default, bool has_send, bool has_timeout,
                              XiValue **block_channels, int block_channel_count, XiBlock *try_head,
                              XiBlock *merge, int line) {
    if (!l->cur_block || l->cur_block == merge)
        return;
    if (has_default) {
        xi_block_set_jump(l->cur_block, merge);
        return;
    }
    if ((!has_send || has_timeout) && block_channel_count > 0) {
        if (block_channel_count > XI_LOWER_MAX_SELECT_CASES) {
            fprintf(stderr, "[LOWER] select block channel count exceeds %d at line %d\n",
                    XI_LOWER_MAX_SELECT_CASES, line);
            l->had_error = true;
            return;
        }
        XiValue *block = xi_value_new(l->func, l->cur_block, XI_SELECT_BLOCK, l->type_unit,
                                      (uint16_t) block_channel_count);
        if (block) {
            block->aux_int = block_channel_count;
            block->line = (uint32_t) line;
            for (int i = 0; i < block_channel_count; i++)
                block->args[i] = block_channels[i];
        }
    } else {
        XiValue *yield = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_unit, 0);
        if (yield) {
            yield->flags |= XI_FLAG_SIDE_EFFECT;
            yield->line = (uint32_t) line;
        }
    }
    xi_block_set_jump(l->cur_block, try_head ? try_head : merge);
}

XR_FUNC void xi_lower_select(XiLower *l, AstNode *node) {
    SelectStmtNode *sel = &node->as.select_stmt;
    int n = sel->case_count;
    if (!lower_select_validate_case_count(l, n, node->line))
        return;

    XiBlock *merge = xi_block_new(l->func);
    bool has_default_case = false;
    bool has_timeout_case = false;
    bool has_send_case = false;
    bool blocking_select = false;
    XiBlock *try_head = NULL;
    XiValue *stack_case_channels[XI_LOWER_SELECT_STACK_CAP] = {0};
    XiValue *stack_case_send_values[XI_LOWER_SELECT_STACK_CAP] = {0};
    uint8_t stack_case_send_modes[XI_LOWER_SELECT_STACK_CAP] = {0};
    XiValue *stack_block_channels[XI_LOWER_SELECT_STACK_CAP];
    LowerSelectLists lists;
    XiValue *timer_chan_val = NULL;  // `after` timer channel; disposed at merge (design/885)
    if (!lower_select_lists_init(l, &lists, n, stack_case_channels, stack_case_send_values,
                                 stack_case_send_modes, stack_block_channels))
        return;
    for (int i = 0; i < n; i++) {
        SelectCaseNode *sc = &sel->cases[i]->as.select_case;
        if (sc->is_default)
            has_default_case = true;
        if (sc->is_timeout)
            has_timeout_case = true;
        if (sc->is_send)
            has_send_case = true;
    }

    blocking_select = !has_default_case;
    if (blocking_select) {
        for (int i = 0; i < n; i++) {
            SelectCaseNode *sc = &sel->cases[i]->as.select_case;
            if (sc->is_default)
                continue;
            if (sc->is_timeout) {
                lists.case_channels[i] = lower_select_time_after(l, sc, sel->cases[i]->line);
                timer_chan_val = lists.case_channels[i];
                continue;
            }
            lists.case_channels[i] = xi_lower_expr(l, sc->channel);
            if (sc->is_send)
                xi_lower_boundary_transfer_arg(l, sc->value, &lists.case_send_values[i],
                                               &lists.case_send_modes[i]);
            if (!l->cur_block)
                return;
        }

        try_head = xi_block_new(l->func);
        if (!try_head)
            return;
        xi_block_set_jump(l->cur_block, try_head);
        l->cur_block = try_head;
    }

    // Create the `after` timer up front (non-blocking selects skip the pre-pass)
    // so every case body can dispose it before any non-local exit. See design/885.
    if (!timer_chan_val) {
        for (int i = 0; i < n; i++) {
            SelectCaseNode *sc = &sel->cases[i]->as.select_case;
            if (sc->is_timeout) {
                lists.case_channels[i] = lower_select_time_after(l, sc, sel->cases[i]->line);
                timer_chan_val = lists.case_channels[i];
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        AstNode *case_node = sel->cases[i];
        SelectCaseNode *sc = &case_node->as.select_case;

        if (sc->is_default) {
            lower_select_emit_timer_dispose(l, timer_chan_val, node->line);
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);

            if (sc->is_send) {
                XiValue *chan =
                    lists.case_channels[i] ? lists.case_channels[i] : xi_lower_expr(l, sc->channel);
                XiValue *val = lists.case_send_values[i];
                uint8_t send_mode = lists.case_send_modes[i];
                if (!val)
                    xi_lower_boundary_transfer_arg(l, sc->value, &val, &send_mode);
                if (chan && val) {
                    XiValue *send =
                        xi_value_new(l->func, l->cur_block, XI_CHAN_TRY_SEND, l->type_bool, 2);
                    if (send) {
                        send->args[0] = chan;
                        send->args[1] = val;
                        send->flags |= XI_FLAG_SIDE_EFFECT;
                        xi_chan_send_set_transfer_mode(send, send_mode);
                        xi_block_set_if(l->cur_block, send, body_blk, next_blk);
                    }
                }
            } else {
                XiValue *chan = NULL;
                if (sc->is_timeout)
                    chan = lists.case_channels[i] ? lists.case_channels[i]
                                                  : lower_select_time_after(l, sc, case_node->line);
                else
                    chan = lists.case_channels[i] ? lists.case_channels[i]
                                                  : xi_lower_expr(l, sc->channel);
                if (chan) {
                    if (!lower_select_block_channel_add(l, &lists, chan, case_node->line))
                        return;
                    struct XrType *val_type = l->type_any;
                    XiValue *recv =
                        xi_value_new(l->func, l->cur_block, XI_CHAN_TRY_RECV, val_type, 1);
                    if (recv) {
                        recv->args[0] = chan;
                        recv->flags |= XI_FLAG_SIDE_EFFECT;
                    }
                    XiValue *recv_status = lower_chan_recv_status(l, recv);
                    lower_select_recv_ready_branch(l, recv_status, chan, body_blk, next_blk);
                    if (sc->var_name && recv) {
                        int var_id =
                            xi_lower_var_create(l, sc->var_symbol_id, sc->var_name, val_type);
                        xi_lower_braun_write(l, var_id, body_blk, recv);
                    }
                }
            }

            xi_lower_braun_seal(l, body_blk);
            xi_lower_braun_seal(l, next_blk);

            l->cur_block = body_blk;
            lower_select_emit_timer_dispose(l, timer_chan_val, node->line);
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);

            l->cur_block = next_blk;
        }
    }

    lower_select_park(l, has_default_case, has_send_case, has_timeout_case, lists.block_channels,
                      lists.block_channel_count, try_head, merge, node->line);

    if (try_head)
        xi_lower_braun_seal(l, try_head);
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

/* ========== Scope Block ========== */

XR_FUNC XiValue *xi_lower_scope_block(XiLower *l, AstNode *node) {
    ScopeBlockNode *sb = &node->as.scope_block;

    XiValue *enter = xi_value_new(l->func, l->cur_block, XI_SCOPE_ENTER, l->type_unit, 0);
    if (enter) {
        enter->aux_int = sb->scope_mode;
        enter->flags |= XI_FLAG_SIDE_EFFECT;
        enter->line = (uint32_t) node->line;
    }

    xi_lower_stmt(l, sb->body);

    /* A scope block is always a statement — XI_SCOPE_EXIT is unit-typed and
     * its register is never read. */
    XiValue *exit_v = xi_value_new(l->func, l->cur_block, XI_SCOPE_EXIT, l->type_unit, 0);
    if (exit_v) {
        exit_v->aux_int = sb->scope_mode;
        exit_v->flags |= XI_FLAG_SIDE_EFFECT;
        exit_v->line = (uint32_t) node->line;
    }

    /* A linked scope re-raises the first child failure.  Value-channel
     * (enum) failures land in pending_error at XI_SCOPE_EXIT; route them to
     * the enclosing catch (inside try) or propagate them (fallible fn), the
     * same way a fallible call does.  Panic-channel child failures unwind
     * inside OP_SCOPE_EXIT and never reach here. */
    if (sb->scope_mode == 1 /* XR_SCOPE_LINKED */)
        xi_lower_insert_err_check(l, node, true);

    return exit_v;
}

/* ========== Pattern Test ========== */

/* True iff the pattern can be reached as part of a tuple slot and acts
 * purely as a binding/wildcard — no equality test, just a name capture
 * that always matches. */
static bool pattern_is_irrefutable_binding(AstNode *pattern) {
    if (!pattern)
        return true;
    if (pattern->type == AST_PATTERN_WILDCARD)
        return true;
    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        if (pval && pval->type == AST_VARIABLE)
            return true;
    }
    return false;
}

/* Binding extraction that is only valid after the pattern test succeeds.
 * Array element reads can trap before the length check. ADT payload slots are
 * variant-typed, so loading them before the tag check lets optimizers merge
 * physically identical slots with incompatible static types. Defer both, even
 * when nested in another structural pattern. */
static bool pattern_bindings_require_match(AstNode *pattern) {
    if (!pattern)
        return false;
    switch (pattern->type) {
        case AST_PATTERN_ARRAY:
        case AST_PATTERN_ADT:
            return true;
        case AST_PATTERN_TUPLE: {
            PatternTupleNode *tuple = &pattern->as.pattern_tuple;
            for (int i = 0; i < tuple->count; i++) {
                if (pattern_bindings_require_match(tuple->patterns[i]))
                    return true;
            }
            return false;
        }
        case AST_PATTERN_OBJECT: {
            PatternObjectNode *object = &pattern->as.pattern_object;
            for (int i = 0; i < object->count; i++) {
                if (pattern_bindings_require_match(object->patterns[i]))
                    return true;
            }
            return false;
        }
        case AST_PATTERN_MULTI: {
            PatternMultiNode *multi = &pattern->as.pattern_multi;
            for (int i = 0; i < multi->count; i++) {
                if (pattern_bindings_require_match(multi->patterns[i]))
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool pattern_payload_is_irrefutable(AstNode *pattern) {
    if (pattern_is_irrefutable_binding(pattern))
        return true;
    if (!pattern)
        return true;
    if (pattern->type == AST_PATTERN_TUPLE) {
        PatternTupleNode *tp = &pattern->as.pattern_tuple;
        for (int i = 0; i < tp->count; i++) {
            if (!pattern_payload_is_irrefutable(tp->patterns[i]))
                return false;
        }
        return true;
    }
    return false;
}

static bool lower_match_mark_exhaustive_adt_pattern(XiLower *l, struct XrType *subject_type,
                                                    AstNode *pattern, bool *covered,
                                                    int member_count, int *covered_count) {
    if (!pattern || !covered || !covered_count)
        return false;
    if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *mp = &pattern->as.pattern_multi;
        bool marked = false;
        for (int i = 0; i < mp->count; i++) {
            if (lower_match_mark_exhaustive_adt_pattern(l, subject_type, mp->patterns[i], covered,
                                                        member_count, covered_count))
                marked = true;
        }
        return marked;
    }
    if (pattern->type != AST_PATTERN_ADT)
        return false;

    PatternAdtNode *ap = &pattern->as.pattern_adt;
    for (int i = 0; i < ap->count; i++) {
        if (!pattern_payload_is_irrefutable(ap->patterns[i]))
            return false;
    }

    int member_index = stmt_adt_member_index(l, subject_type, ap->variant);
    if (member_index < 0 || member_index >= member_count)
        return false;
    if (!covered[member_index]) {
        covered[member_index] = true;
        (*covered_count)++;
    }
    return true;
}

static bool lower_match_is_exhaustive_adt(XiLower *l, struct XrType *subject_type,
                                          MatchExprNode *m) {
    if (!l || !m)
        return false;
    XaSymbolLinks *links = stmt_adt_subject_links(l, subject_type);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (!info || !info->is_payload_enum || info->variant_count == 0 || info->variant_count > 256)
        return false;

    bool stack_covered[256];
    memset(stack_covered, 0, sizeof(stack_covered));
    int covered_count = 0;
    for (int i = 0; i < m->arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        if (!arm_node || arm_node->type != AST_MATCH_ARM)
            return false;
        MatchArmNode *arm = &arm_node->as.match_arm;
        if (arm->guard)
            continue;
        if (pattern_is_irrefutable_binding(arm->pattern))
            return true;
        lower_match_mark_exhaustive_adt_pattern(l, subject_type, arm->pattern, stack_covered,
                                                (int) info->variant_count, &covered_count);
        if (covered_count == (int) info->variant_count)
            return true;
    }
    return false;
}

/* Read object/Json field `fname` from `subject` (static Json index when known,
 * else a dynamic string-keyed index get; both null-safe on a missing field).
 * `source_span_id` keys the verified object-access row the pattern's producer
 * evidence recorded for this field, mirroring destructure statements. */
static XiValue *lower_match_field_get(XiLower *l, XiValue *subject, const char *fname,
                                      uint32_t source_span_id) {
    int fidx = subject->type ? stmt_json_field_index(subject->type, fname) : -1;
    if (fidx >= 0) {
        struct XrType *ft =
            subject->type->object.field_types ? subject->type->object.field_types[fidx] : NULL;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_OBJECT_GET_F, ft ? ft : l->type_any, 1);
        if (v) {
            v->args[0] = subject;
            v->aux_int = fidx;
            v->line = source_span_id;
            xi_lower_bind_object_access_id(l, v, fname, source_span_id,
                                           XG_OBJECT_ACCESS_DESTRUCTURE);
        }
        return v;
    }
    XiValue *key = xi_const_str(l->func, l->cur_block, fname, l->type_string);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, l->type_any, 2);
    if (v) {
        v->args[0] = subject;
        v->args[1] = key;
    }
    return v;
}

/* Read array element at index `idx` (caller must have verified bounds). */
static XiValue *lower_match_elem_get(XiLower *l, XiValue *subject, int idx) {
    struct XrType *et =
        (subject->type && (XR_TYPE_IS_ARRAY(subject->type) || XR_TYPE_IS_SLICE(subject->type) ||
                           XR_TYPE_IS_SLICE(subject->type)))
            ? subject->type->container.element_type
            : NULL;
    XiValue *iv = xi_const_int(l->func, l->cur_block, idx, l->type_int);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, et ? et : l->type_any, 2);
    if (v) {
        v->args[0] = subject;
        v->args[1] = iv;
    }
    return v;
}

/* Array length of `subject`. */
static XiValue *lower_match_array_len(XiLower *l, XiValue *subject) {
    XiValue *len = xi_value_new(l->func, l->cur_block, XI_LEN, l->type_int, 1);
    if (!len)
        return NULL;
    len->args[0] = subject;
    return len;
}

/* Resolve the static element type for tuple slot `idx`. Falls back to
 * `type_any` when the analyzer hasn't proven a tuple type for the
 * subject (e.g. the source uses Json or untyped values). */
static struct XrType *tuple_elem_type(XiLower *l, struct XrType *subject_type, int idx) {
    if (subject_type) {
        struct XrType *et = xr_type_tuple_get(subject_type, idx);
        if (et)
            return et;
    }
    return l->type_any;
}

/* Variant-ordinal test for an ADT-enum value: read logical ADT field 0 (the
 * tag) and compare it against the pattern variant's static ordinal. Logical
 * ADT fields are lowered through enum aggregate helpers in the VM/AOT backend,
 * so this numbering is an IR convention, not a runtime XrInstance layout. */
static XiValue *lower_adt_variant_tag_test(XiLower *l, XiValue *subject, AstNode *variant) {
    int member_index = stmt_adt_member_index(l, subject->type, variant);
    struct XrType *tag_type = member_index >= 0 ? l->type_int : l->type_any;
    XiValue *tag = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, tag_type, 1);
    if (!tag)
        return NULL;
    tag->args[0] = subject;
    tag->aux_int = 0; /* logical ADT field 0 = variant tag */
    tag->aux_kind = XI_AUX_KIND_ADT_FIELD;

    if (member_index >= 0) {
        XiValue *want = xi_const_int(l->func, l->cur_block, member_index, l->type_int);
        if (!want)
            return NULL;
        return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, tag, want);
    }

    XiValue *variant_val = xi_lower_expr(l, variant);
    if (!variant_val)
        return NULL;
    return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, tag, variant_val);
}

/* A bare enum-member pattern (`Recv.Empty`, no payload parens) parses as a
 * literal. When the subject is an ADT enum, it must still test the variant
 * ordinal rather than compare against the variant singleton, otherwise the
 * tagged-instance representation makes the match always fall through. */
static bool lower_literal_is_adt_variant(XiLower *l, XiValue *subject, AstNode *value) {
    if (!value)
        return false;
    if (value->type != AST_ENUM_ACCESS && value->type != AST_MEMBER_ACCESS)
        return false;
    XaSymbolLinks *links = stmt_adt_subject_links(l, subject->type);
    if (!links || !links->enum_info || !links->enum_info->is_payload_enum)
        return false;
    return stmt_adt_member_index(l, subject->type, value) >= 0;
}

XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject)
        return NULL;

    switch (pattern->type) {
        case AST_PATTERN_WILDCARD:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

        case AST_PATTERN_LITERAL: {
            /* A bare AST_VARIABLE literal at this depth is a nested
             * binding (e.g. inside `(0, x)`); its match test is
             * unconditional — the actual capture is performed by
             * lower_pattern_bindings before the test runs. */
            AstNode *pval = pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE)
                return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

            if (lower_literal_is_adt_variant(l, subject, pval))
                return lower_adt_variant_tag_test(l, subject, pval);

            XiValue *lit = xi_lower_expr(l, pattern->as.pattern_literal.value);
            if (!lit)
                return NULL;
            return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, subject, lit);
        }

        case AST_PATTERN_RANGE: {
            XiValue *start = xi_lower_expr(l, pattern->as.pattern_range.start);
            XiValue *end = xi_lower_expr(l, pattern->as.pattern_range.end);
            if (!start || !end)
                return NULL;
            XiValue *ge = xi_binary(l->func, l->cur_block, XI_GE, l->type_bool, subject, start);
            XiValue *upper = xi_binary(l->func, l->cur_block,
                                       pattern->as.pattern_range.inclusive_end ? XI_LE : XI_LT,
                                       l->type_bool, subject, end);
            return xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, ge, upper);
        }

        case AST_PATTERN_MULTI: {
            PatternMultiNode *mp = &pattern->as.pattern_multi;
            XiValue *result = NULL;
            for (int i = 0; i < mp->count; i++) {
                XiValue *test = xi_lower_pattern_test(l, subject, mp->patterns[i]);
                if (!test)
                    continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BOR, l->type_bool, result, test);
            }
            return result ? result : xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        }

        case AST_PATTERN_TUPLE: {
            /* Per-slot conjunction: TUPLE_GET each refutable slot and
             * AND its sub-test. Irrefutable slots (wildcard / binding)
             * contribute nothing to the test — they are always true and
             * folding them in would just bloat the IR. */
            PatternTupleNode *tp = &pattern->as.pattern_tuple;
            XiValue *result = NULL;
            for (int i = 0; i < tp->count; i++) {
                AstNode *sub = tp->patterns[i];
                if (pattern_is_irrefutable_binding(sub))
                    continue;
                struct XrType *et = tuple_elem_type(l, subject->type, i);
                XiValue *get = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et, 1);
                if (!get)
                    return NULL;
                get->args[0] = subject;
                get->aux_int = i;
                XiValue *test = xi_lower_pattern_test(l, get, sub);
                if (!test)
                    continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, result, test);
            }
            /* All-irrefutable tuple pattern (e.g. `(_, _)`) matches anything
             * the analyzer let through — emit a constant true. */
            return result ? result : xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        }

        case AST_PATTERN_ADT: {
            /* ADT variant destructure: field[0] stores the variant ordinal.
             * Prefer a static ordinal compare so VM and AOT avoid dynamic enum
             * member lookup in every pattern test. */
            PatternAdtNode *ap = &pattern->as.pattern_adt;
            return lower_adt_variant_tag_test(l, subject, ap->variant);
        }

        case AST_PATTERN_OBJECT: {
            /* Object pattern: AND of refutable field sub-tests. Field reads are
             * null-safe (missing field -> null), so they are safe in the test;
             * irrefutable field sub-patterns (bindings/wildcards) contribute
             * nothing and are captured by lower_pattern_bindings. */
            PatternObjectNode *op = &pattern->as.pattern_object;
            XiValue *result = NULL;
            for (int i = 0; i < op->count; i++) {
                AstNode *sub = op->patterns[i];
                if (pattern_is_irrefutable_binding(sub))
                    continue;
                XiValue *fv =
                    lower_match_field_get(l, subject, op->field_names[i], (uint32_t) pattern->line);
                if (!fv)
                    continue;
                XiValue *t = xi_lower_pattern_test(l, fv, sub);
                if (!t)
                    continue;
                result =
                    result ? xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, result, t) : t;
            }
            return result ? result : xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        }

        case AST_PATTERN_ARRAY: {
            /* Array pattern: the test is the length check only. Element reads
             * trap out of bounds, so element bindings are deferred to the body
             * block (after the length test passes); element sub-patterns are
             * restricted to bindings/wildcards by the analyzer. */
            PatternArrayNode *ap = &pattern->as.pattern_array;
            XiValue *len = lower_match_array_len(l, subject);
            if (!len)
                return NULL;
            XiValue *cnt = xi_const_int(l->func, l->cur_block, ap->count, l->type_int);
            return xi_binary(l->func, l->cur_block, ap->has_rest ? XI_GE : XI_EQ, l->type_bool, len,
                             cnt);
        }

        case AST_PATTERN_TYPE: {
            /* `is T [name]`: runtime type test against T. The binding (if
             * present) is captured in lower_pattern_bindings once the
             * test succeeds. */
            PatternTypeNode *tp = &pattern->as.pattern_type;
            return xi_lower_is_test(l, subject, tp->type, pattern->line, 0);
        }

        default:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
    }
}

/* Walk the pattern tree and bind every AST_VARIABLE leaf to the
 * corresponding subject value. Tuple sub-patterns reach their slot via
 * a fresh XI_TUPLE_GET; subsequent loads of the same slot get folded
 * by the const_fold tuple-projection peephole when paired with a
 * TUPLE_NEW source. */
static void lower_pattern_bindings(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject)
        return;

    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        if (pval && pval->type == AST_VARIABLE) {
            const char *bname = pval->as.variable.name;
            uint32_t bsid = pval->as.variable.symbol_id;
            int var_id =
                xi_lower_var_create(l, bsid, bname, subject->type ? subject->type : l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, subject);
        }
        return;
    }

    if (pattern->type == AST_PATTERN_TUPLE) {
        PatternTupleNode *tp = &pattern->as.pattern_tuple;
        for (int i = 0; i < tp->count; i++) {
            AstNode *sub = tp->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            struct XrType *et = tuple_elem_type(l, subject->type, i);
            XiValue *get = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et, 1);
            if (!get)
                continue;
            get->args[0] = subject;
            get->aux_int = i;
            lower_pattern_bindings(l, get, sub);
        }
    }

    /* ADT variant destructure: bind logical payload fields.
     * Logical field 0 is the tag; payloads start at logical field 1. */
    if (pattern->type == AST_PATTERN_ADT) {
        PatternAdtNode *ap = &pattern->as.pattern_adt;
        for (int i = 0; i < ap->count; i++) {
            AstNode *sub = ap->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            struct XrType *payload_type =
                xa_analyzer_resolve_adt_payload_type(l->analyzer, subject->type, ap->variant, i);
            XiValue *field = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD,
                                          payload_type ? payload_type : l->type_any, 1);
            if (!field)
                continue;
            field->args[0] = subject;
            field->aux_int = 1 + i; /* logical ADT payload field */
            field->aux_kind = XI_AUX_KIND_ADT_FIELD;
            lower_pattern_bindings(l, field, sub);
        }
    }

    /* Object pattern: bind each field's sub-pattern to the field value. */
    if (pattern->type == AST_PATTERN_OBJECT) {
        PatternObjectNode *op = &pattern->as.pattern_object;
        for (int i = 0; i < op->count; i++) {
            AstNode *sub = op->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            XiValue *fv =
                lower_match_field_get(l, subject, op->field_names[i], (uint32_t) pattern->line);
            if (!fv)
                continue;
            lower_pattern_bindings(l, fv, sub);
        }
    }

    /* Array pattern: bind positional elements (length already verified by the
     * test) and the optional rest as a tail slice. */
    if (pattern->type == AST_PATTERN_ARRAY) {
        PatternArrayNode *ap = &pattern->as.pattern_array;
        for (int i = 0; i < ap->count; i++) {
            AstNode *sub = ap->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            XiValue *ev = lower_match_elem_get(l, subject, i);
            if (ev)
                lower_pattern_bindings(l, ev, sub);
        }
        if (ap->rest_name) {
            struct XrType *rest_type = l->type_any;
            if (subject->type && XR_TYPE_IS_SLICE(subject->type)) {
                rest_type = subject->type;
            } else if (subject->type && XR_TYPE_IS_SLICE(subject->type)) {
                rest_type = subject->type;
            } else if (subject->type && XR_TYPE_IS_ARRAY(subject->type)) {
                struct XrType *elem = subject->type->container.element_type;
                rest_type = xr_type_new_slice(l->isolate, elem ? elem : xr_type_new_unknown(NULL));
            }
            XiValue *start = xi_const_int(l->func, l->cur_block, ap->count, l->type_int);
            XiValue *end = lower_match_array_len(l, subject);
            XiValue *slice = xi_value_new(l->func, l->cur_block, XI_SLICE, rest_type, 3);
            if (slice && end) {
                slice->args[0] = subject;
                slice->args[1] = start;
                slice->args[2] = end;
                int var_id = xi_lower_var_create(l, ap->rest_symbol_id, ap->rest_name, rest_type);
                xi_lower_braun_write(l, var_id, l->cur_block, slice);
            }
        }
    }

    /* Type pattern: bind the narrowed name (if any) to the subject. The
     * subject's static type is the union; the binding sees only the
     * matching arm and is typed as T by the analyzer. */
    if (pattern->type == AST_PATTERN_TYPE) {
        PatternTypeNode *tp = &pattern->as.pattern_type;
        if (tp->binding_name) {
            int var_id = xi_lower_var_create(l, tp->symbol_id, tp->binding_name,
                                             subject->type ? subject->type : l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, subject);
        }
    }
}

/* ========== Match Expression ========== */

static void lower_match_no_match_throw(XiLower *l, int line) {
    if (!l || !l->cur_block)
        return;

    struct XrType *exception_type = xr_type_new_class(NULL, "PanicInfo");
    XiValue *cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, exception_type, 0);
    if (!cls)
        return;
    cls->aux_int = XR_GLOBAL_VAR_PANIC_INFO;
    cls->aux = (void *) "PanicInfo";

    XiValue *msg =
        xi_const_str(l->func, l->cur_block, "E0442: non-exhaustive match", l->type_string);
    XiValue *exc = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, exception_type, 2);
    if (!exc)
        return;
    exc->args[0] = cls;
    exc->args[1] = msg;
    exc->aux = (void *) "constructor";
    exc->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    exc->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    exc->line = (uint32_t) line;

    XiValue *thr = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_unit, 1);
    if (!thr)
        return;
    thr->args[0] = exc;
    thr->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    thr->line = (uint32_t) line;
    l->cur_block->kind = XI_BLOCK_UNREACHABLE;
    l->cur_block->control = exc;
    l->cur_block = NULL;
}

static bool match_recv_value_pattern(AstNode *pattern, AstNode **payload_out) {
    if (payload_out)
        *payload_out = NULL;
    if (!pattern || pattern->type != AST_PATTERN_ADT)
        return false;
    PatternAdtNode *ap = &pattern->as.pattern_adt;
    if (ap->count != 1 || !ap->variant || ap->variant->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &ap->variant->as.member_access;
    if (!ma->name || strcmp(ma->name, "Value") != 0)
        return false;
    if (!ma->object || ma->object->type != AST_VARIABLE)
        return false;
    if (strcmp(ma->object->as.variable.name, "Recv") != 0)
        return false;
    if (payload_out)
        *payload_out = ap->patterns ? ap->patterns[0] : NULL;
    return true;
}

static bool match_pattern_is_wildcard(AstNode *pattern) {
    return pattern && pattern->type == AST_PATTERN_WILDCARD;
}

static bool match_channel_recv_subject(XiLower *l, AstNode *expr, AstNode **chan_expr_out,
                                       bool *try_recv_out) {
    if (chan_expr_out)
        *chan_expr_out = NULL;
    if (try_recv_out)
        *try_recv_out = false;
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;

    CallExprNode *call = &expr->as.call_expr;
    if (call->arg_count != 0 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->object || !ma->name)
        return false;

    bool is_recv = strcmp(ma->name, "recv") == 0;
    bool is_try_recv = strcmp(ma->name, "tryRecv") == 0;
    if (!is_recv && !is_try_recv)
        return false;

    struct XrType *recv_type = xa_analyzer_get_node_type(l->analyzer, ma->object);
    if (!stmt_type_is_channel(recv_type))
        return false;

    if (chan_expr_out)
        *chan_expr_out = ma->object;
    if (try_recv_out)
        *try_recv_out = is_try_recv;
    return true;
}

static bool match_channel_recv_fast_supported(MatchExprNode *m) {
    bool saw_value = false;
    for (int i = 0; i < m->arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        if (!arm_node || arm_node->type != AST_MATCH_ARM)
            return false;
        MatchArmNode *arm = &arm_node->as.match_arm;
        AstNode *payload = NULL;
        if (match_recv_value_pattern(arm->pattern, &payload)) {
            if (!pattern_is_irrefutable_binding(payload))
                return false;
            saw_value = true;
            continue;
        }
        if (!match_pattern_is_wildcard(arm->pattern))
            return false;
    }
    return saw_value;
}

#define XI_LOWER_MATCH_EXIT_STACK_CAP 32
#define XI_LOWER_MAX_MATCH_ARMS ((int) UINT16_MAX - 1)

typedef struct LowerMatchExitList {
    XiBlock **blocks;
    XiValue **values;
    int count;
    int cap;
} LowerMatchExitList;

static bool lower_match_validate_arm_count(XiLower *l, int arm_count, int line) {
    if (arm_count < 0 || arm_count > XI_LOWER_MAX_MATCH_ARMS) {
        fprintf(stderr, "[LOWER] match arm count exceeds %d at line %d\n", XI_LOWER_MAX_MATCH_ARMS,
                line);
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_match_exit_list_init(XiLower *l, LowerMatchExitList *list, int arm_count,
                                       XiBlock **stack_blocks, XiValue **stack_values) {
    int cap = arm_count > 0 ? arm_count : 1;
    list->count = 0;
    list->cap = cap;
    if (cap <= XI_LOWER_MATCH_EXIT_STACK_CAP) {
        list->blocks = stack_blocks;
        list->values = stack_values;
        return true;
    }

    list->blocks =
        (XiBlock **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiBlock *)));
    list->values =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    if (!list->blocks || !list->values) {
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_match_exit_list_add(XiLower *l, LowerMatchExitList *list, XiBlock *block,
                                      XiValue *value, int line) {
    if (list->count >= list->cap) {
        fprintf(stderr, "[LOWER] match exit count exceeds %d at line %d\n", list->cap, line);
        l->had_error = true;
        return false;
    }
    list->blocks[list->count] = block;
    list->values[list->count] = value;
    list->count++;
    return true;
}

static XiValue *lower_channel_recv_match_phi(XiLower *l, XiBlock *merge, struct XrType *result_type,
                                             const LowerMatchExitList *exits) {
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block)
        return NULL;

    if (merge->npreds == 1)
        return (exits->count > 0) ? exits->values[0] : NULL;

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi)
        return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exits->count; j++) {
            if (merge->preds[p] == exits->blocks[j]) {
                phi->value.args[p] = exits->values[j] ? exits->values[j]
                                                      : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

static bool lower_channel_recv_match(XiLower *l, AstNode *node, XiValue **out_value) {
    if (out_value)
        *out_value = NULL;
    MatchExprNode *m = &node->as.match_expr;
    AstNode *chan_expr = NULL;
    bool is_try_recv = false;
    if (!match_channel_recv_subject(l, m->expr, &chan_expr, &is_try_recv))
        return false;
    if (!match_channel_recv_fast_supported(m))
        return false;
    if (!lower_match_validate_arm_count(l, m->arm_count, node->line))
        return true;

    XiValue *chan = xi_lower_expr(l, chan_expr);
    if (!chan || !l->cur_block)
        return true;

    struct XrType *payload_type = stmt_channel_element_type(chan->type);
    if (!payload_type)
        payload_type = l->type_any;

    XiValue *recv = xi_value_new(l->func, l->cur_block,
                                 is_try_recv ? XI_CHAN_TRY_RECV : XI_CHAN_RECV, payload_type, 1);
    if (!recv)
        return true;
    recv->args[0] = chan;
    recv->flags |= XI_FLAG_SIDE_EFFECT;
    if (!is_try_recv)
        recv->flags |= XI_FLAG_MAY_SUSPEND;
    recv->line = (uint32_t) node->line;

    XiValue *recv_status = lower_chan_recv_status(l, recv);
    if (!recv_status)
        return true;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *stack_body_exits[XI_LOWER_MATCH_EXIT_STACK_CAP];
    XiValue *stack_body_vals[XI_LOWER_MATCH_EXIT_STACK_CAP];
    LowerMatchExitList exits;
    if (!lower_match_exit_list_init(l, &exits, m->arm_count, stack_body_exits, stack_body_vals))
        return true;

    for (int i = 0; i < m->arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;
        AstNode *payload = NULL;
        bool is_value_arm = match_recv_value_pattern(arm->pattern, &payload);

        XiValue *test = NULL;
        if (is_value_arm) {
            lower_pattern_bindings(l, recv, payload);
            test = recv_status;
            if (arm->guard) {
                XiValue *guard = lower_guard_expr(l, arm->guard);
                if (guard)
                    test = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, test, guard);
            }
        } else if (arm->guard) {
            test = lower_guard_expr(l, arm->guard);
        }

        if (!test) {
            XiValue *val = NULL;
            if (arm->body && arm->body->type == AST_BLOCK) {
                xi_lower_stmt(l, arm->body);
                if (l->cur_block)
                    val = xi_const_null(l->func, l->cur_block, l->type_null);
            } else {
                val = xi_lower_expr(l, arm->body);
            }
            if (l->cur_block) {
                if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                    return true;
                xi_block_set_jump(l->cur_block, merge);
            }
            l->cur_block = NULL;
            break;
        }

        XiBlock *body_blk = xi_block_new(l->func);
        XiBlock *next_blk = xi_block_new(l->func);
        xi_block_set_if(l->cur_block, test, body_blk, next_blk);
        xi_lower_braun_seal(l, body_blk);
        xi_lower_braun_seal(l, next_blk);

        l->cur_block = body_blk;
        XiValue *val = NULL;
        if (arm->body && arm->body->type == AST_BLOCK) {
            xi_lower_stmt(l, arm->body);
            if (l->cur_block)
                val = xi_const_null(l->func, l->cur_block, l->type_null);
        } else {
            val = xi_lower_expr(l, arm->body);
        }
        if (l->cur_block) {
            if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                return true;
            xi_block_set_jump(l->cur_block, merge);
        }

        l->cur_block = next_blk;
    }

    if (l->cur_block && l->cur_block != merge)
        lower_match_no_match_throw(l, node->line);

    if (out_value)
        *out_value = lower_channel_recv_match_phi(l, merge, result_type, &exits);
    return true;
}

/* Lower a match arm body, record its exit value, and jump to the merge block.
 * Returns false on allocation failure. */
static bool lower_match_emit_arm_body(XiLower *l, MatchArmNode *arm, LowerMatchExitList *exits,
                                      XiBlock *merge, int line) {
    XiValue *val = NULL;
    if (arm->body && arm->body->type == AST_BLOCK) {
        xi_lower_stmt(l, arm->body);
        if (l->cur_block)
            val = xi_const_null(l->func, l->cur_block, l->type_null);
    } else {
        val = xi_lower_expr(l, arm->body);
    }
    if (l->cur_block) {
        if (!lower_match_exit_list_add(l, exits, l->cur_block, val, line))
            return false;
        xi_block_set_jump(l->cur_block, merge);
    }
    return true;
}

XR_FUNC XiValue *xi_lower_match(XiLower *l, AstNode *node) {
    MatchExprNode *m = &node->as.match_expr;
    XiValue *fast_value = NULL;
    if (lower_channel_recv_match(l, node, &fast_value))
        return fast_value;

    XiValue *subject = xi_lower_expr(l, m->expr);
    if (!subject)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    int arm_count = m->arm_count;
    if (!lower_match_validate_arm_count(l, arm_count, node->line))
        return NULL;
    bool exhaustive_adt = lower_match_is_exhaustive_adt(l, subject->type, m);
    XiBlock *stack_body_exits[XI_LOWER_MATCH_EXIT_STACK_CAP];
    XiValue *stack_body_vals[XI_LOWER_MATCH_EXIT_STACK_CAP];
    LowerMatchExitList exits;
    if (!lower_match_exit_list_init(l, &exits, arm_count, stack_body_exits, stack_body_vals))
        return NULL;

    for (int i = 0; i < arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;

        /* Bind every named slot in the pattern (top-level bare name or
         * AST_VARIABLEs nested inside a tuple pattern) before lowering the
         * test or guard when extraction is unconditionally safe.
         *
         * is_top_binding is the legacy "bare-name pattern" case where
         * the match test reduces to TRUE and selection is decided
         * entirely by the optional guard. Tuple patterns don't get
         * that shortcut: their refutable slots still need TUPLE_GET-
         * based equality testing.
         *
         * Array and ADT patterns defer their bindings (and any guard reading
         * them) to the matched body block: array reads require a successful
         * length test, and ADT payload types require a successful tag test. */
        bool defer_bindings = pattern_bindings_require_match(arm->pattern);
        if (!defer_bindings)
            lower_pattern_bindings(l, subject, arm->pattern);

        bool is_top_irrefutable = !arm->guard && pattern_is_irrefutable_binding(arm->pattern);
        bool is_top_binding = false;
        if (arm->pattern && arm->pattern->type == AST_PATTERN_LITERAL) {
            AstNode *pval = arm->pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE)
                is_top_binding = true;
        }

        XiValue *test;
        if (is_top_irrefutable) {
            /* Top-level wildcard / bare binding without a guard always matches. */
            test = NULL;
        } else if (is_top_binding) {
            /* Guarded bare-name pattern narrows with the guard expression. */
            test = arm->guard ? lower_guard_expr(l, arm->guard) : NULL;
        } else {
            test = xi_lower_pattern_test(l, subject, arm->pattern);
            /* For deferred patterns the guard is folded in inside the body
             * block (after binding), because it may read captured names. */
            if (!defer_bindings && arm->guard && test) {
                XiValue *guard = lower_guard_expr(l, arm->guard);
                if (guard)
                    test = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, test, guard);
            }
        }

        if (!test) {
            if (!lower_match_emit_arm_body(l, arm, &exits, merge, node->line))
                return NULL;
            l->cur_block = NULL;
            break;
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, test, body_blk, next_blk);
            xi_lower_braun_seal(l, body_blk);

            l->cur_block = body_blk;

            /* Deferred bindings (array patterns): the length test has passed,
             * so element reads are now in bounds. A guard that reads the
             * captures branches back to next_blk on failure. */
            bool next_sealed = false;
            if (defer_bindings) {
                lower_pattern_bindings(l, subject, arm->pattern);
                if (arm->guard) {
                    XiValue *guard = lower_guard_expr(l, arm->guard);
                    if (guard) {
                        XiBlock *guard_body = xi_block_new(l->func);
                        xi_block_set_if(l->cur_block, guard, guard_body, next_blk);
                        xi_lower_braun_seal(l, guard_body);
                        xi_lower_braun_seal(l, next_blk);
                        next_sealed = true;
                        l->cur_block = guard_body;
                    }
                }
            }
            if (!next_sealed)
                xi_lower_braun_seal(l, next_blk);

            if (!lower_match_emit_arm_body(l, arm, &exits, merge, node->line))
                return NULL;

            l->cur_block = next_blk;
        }
    }

    if (l->cur_block && l->cur_block != merge) {
        if (exhaustive_adt) {
            l->cur_block->kind = XI_BLOCK_UNREACHABLE;
            l->cur_block->control = NULL;
            l->cur_block = NULL;
        } else {
            lower_match_no_match_throw(l, node->line);
        }
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block)
        return NULL;

    if (merge->npreds == 1) {
        return (exits.count > 0) ? exits.values[0] : NULL;
    }

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi)
        return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exits.count; j++) {
            if (merge->preds[p] == exits.blocks[j]) {
                phi->value.args[p] =
                    exits.values[j] ? exits.values[j] : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

/* ========== For-In Loop (index-based) ========== */

static void lower_for_in_loop(XiLower *l, AstNode *node, XiValue *init_val, XiValue *limit,
                              XiValue *get_item_coll, bool inclusive_limit) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    (void) s;
    struct XrType *item_type = xi_lower_node_type(l, node);

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__for_idx_%d", sid);
    char *idx_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(idx_name != NULL, "arena alloc failed for idx_name");
    memcpy(idx_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_lim_%d", sid);
    char *lim_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(lim_name != NULL, "arena alloc failed for lim_name");
    memcpy(lim_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_col_%d", sid);
    char *col_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(col_name != NULL, "arena alloc failed for col_name");
    memcpy(col_name, buf, strlen(buf) + 1);

    int idx_var = xi_lower_var_create(l, 0, idx_name, l->type_int);
    int lim_var = xi_lower_var_create(l, 0, lim_name, l->type_int);
    xi_lower_braun_write(l, idx_var, l->cur_block, init_val);
    xi_lower_braun_write(l, lim_var, l->cur_block, limit);

    int col_var = -1;
    if (get_item_coll) {
        col_var = xi_lower_var_create(l, 0, col_name,
                                      get_item_coll->type ? get_item_coll->type : l->type_any);
        xi_lower_braun_write(l, col_var, l->cur_block, get_item_coll);
    }

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *cur_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *cur_lim = xi_lower_braun_read(l, lim_var, l->cur_block);
    XR_DCHECK(cur_idx != NULL, "braun_read idx must not be NULL");
    XiValue *cond = xi_binary(l->func, l->cur_block, inclusive_limit ? XI_LE : XI_LT, l->type_bool,
                              cur_idx, cur_lim);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, incr_blk);

    l->cur_block = body_blk;
    XiValue *body_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *item;

    if (s->domain_kind == XR_FOR_IN_DOMAIN_ENUM_VARIANTS) {
        XiValue *body_limit = xi_lower_braun_read(l, lim_var, l->cur_block);
        item = xi_value_new(l->func, l->cur_block, XI_ENUM_VARIANT_AT, item_type, 2);
        if (item) {
            item->args[0] = body_limit;
            item->args[1] = body_idx;
            item->flags |= XI_FLAG_MAY_THROW;
            item->line = (uint32_t) node->line;
            item->enum_metadata_owner = xr_type_enum_metadata_owner(item_type);
            item->enum_metadata_kind = XR_ENUM_METADATA_VARIANT;
        }
    } else if (s->domain_kind == XR_FOR_IN_DOMAIN_ENUM_PAYLOADS) {
        XiValue *body_col = xi_lower_braun_read(l, col_var, l->cur_block);
        item = xi_value_new(l->func, l->cur_block, XI_ENUM_PAYLOAD_AT, item_type, 2);
        if (item) {
            item->args[0] = body_col;
            item->args[1] = body_idx;
            item->flags |= XI_FLAG_MAY_THROW;
            item->line = (uint32_t) node->line;
            item->enum_metadata_owner = xr_type_enum_metadata_owner(item_type);
            item->enum_metadata_kind = XR_ENUM_METADATA_PAYLOAD_FIELD;
        }
    } else if (get_item_coll) {
        XiValue *body_col = xi_lower_braun_read(l, col_var, l->cur_block);
        item = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, item_type, 2);
        if (item) {
            item->args[0] = body_col;
            item->args[1] = body_idx;
            if (s->domain_kind == XR_FOR_IN_DOMAIN_UNIT_ENUM_VALUES)
                item->aux_kind = XI_AUX_KIND_ENUM_CASE;
            if (s->domain_kind == XR_FOR_IN_DOMAIN_UNIT_ENUM_VALUES)
                item->enum_metadata_owner = item_type;
            item->line = (uint32_t) node->line;
        }
    } else {
        item = body_idx;
    }

    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (item)
        xi_lower_braun_write(l, item_var, l->cur_block, item);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
        if (inclusive_limit) {
            XiValue *inc_lim = xi_lower_braun_read(l, lim_var, l->cur_block);
            XiValue *at_limit =
                xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, inc_idx, inc_lim);
            XiBlock *step_blk = xi_block_new(l->func);
            if (at_limit)
                xi_block_set_if(l->cur_block, at_limit, exit_blk, step_blk);
            xi_lower_braun_seal(l, step_blk);

            l->cur_block = step_blk;
            XiValue *step_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
            XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
            XiValue *new_idx = xi_binary(l->func, l->cur_block, XI_ADD, l->type_int, step_idx, one);
            if (new_idx)
                xi_lower_braun_write(l, idx_var, l->cur_block, new_idx);
        } else {
            XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
            XiValue *new_idx = xi_binary(l->func, l->cur_block, XI_ADD, l->type_int, inc_idx, one);
            if (new_idx)
                xi_lower_braun_write(l, idx_var, l->cur_block, new_idx);
        }
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In Key-Value (iterator protocol) ========== */

static void lower_for_in_keyvalue(XiLower *l, AstNode *node) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    uint32_t line = (uint32_t) node->line;

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block)
        return;

    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!iter)
        return;
    iter->args[0] = coll;
    iter->aux = (void *) "entriesIterator";
    iter->aux_int = (int64_t) xi_lower_method_symbol(l, "entriesIterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__kv_iter_%d", sid);
    char *iter_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(iter_name != NULL, "arena alloc failed");
    memcpy(iter_name, buf, strlen(buf) + 1);
    int iter_var = xi_lower_var_create(l, 0, iter_name, l->type_any);
    xi_lower_braun_write(l, iter_var, l->cur_block, iter);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *iter_cond = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_bool, 1);
    if (!has_next)
        return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = (int64_t) xi_lower_method_symbol(l, "hasNext") << 1;
    has_next->flags |= XI_FLAG_SIDE_EFFECT;
    has_next->line = line;
    xi_block_set_if(l->cur_block, has_next, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, cond_blk);

    l->cur_block = body_blk;
    XiValue *iter_body = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *entry = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!entry)
        return;
    entry->args[0] = iter_body;
    entry->aux = (void *) "next";
    entry->aux_int = (int64_t) xi_lower_method_symbol(l, "next") << 1;
    entry->flags |= XI_FLAG_SIDE_EFFECT;
    entry->line = line;

    struct XrType *item_type = xi_lower_node_type(l, node);

    /* The iterator yields a (key, value) tuple per step (see
     * xr_iterator_next: Map/Json/Array/String all build XrTuple pairs).
     * Read each slot with TUPLE_GET so the access matches the runtime
     * representation; downstream peephole can fold this against a
     * fresh TUPLE_NEW when the source is inlinable. */
    XiValue *key_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, item_type, 1);
    if (key_val) {
        key_val->args[0] = entry;
        key_val->aux_int = 0;
        key_val->line = line;
    }
    int key_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (key_val)
        xi_lower_braun_write(l, key_var, l->cur_block, key_val);

    if (s->value_name) {
        XiValue *val_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, l->type_any, 1);
        if (val_val) {
            val_val->args[0] = entry;
            val_val->aux_int = 1;
            val_val->line = line;
        }
        int val_var = xi_lower_var_create(l, s->value_symbol_id, s->value_name, l->type_any);
        if (val_val)
            xi_lower_braun_write(l, val_var, l->cur_block, val_val);
    }

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In: Custom Iterator Protocol ========== */

/* Lower `for (item in obj)` where obj has an iterator() method returning
 * an object with hasNext(): bool and next(): T.
 *
 * Desugars to:
 *   var __iter = obj.iterator()
 *   while (__iter.hasNext()) {
 *       var item = __iter.next()
 *       <body>
 *   }
 */
static void lower_for_in_custom_iterator(XiLower *l, AstNode *node, XiValue *coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    uint32_t line = (uint32_t) node->line;

    /* Call iterator() on the collection */
    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!iter)
        return;
    iter->args[0] = coll;
    iter->aux = (void *) "iterator";
    iter->aux_int = (int64_t) xi_lower_method_symbol(l, "iterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;
    xi_lower_insert_err_check(l, node, true);
    if (!l->cur_block)
        return;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__ci_iter_%d", sid);
    char *iter_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(iter_name != NULL, "arena alloc failed");
    memcpy(iter_name, buf, strlen(buf) + 1);
    int iter_var = xi_lower_var_create(l, 0, iter_name, l->type_any);
    xi_lower_braun_write(l, iter_var, l->cur_block, iter);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: __iter.hasNext() */
    l->cur_block = cond_blk;
    XiValue *iter_cond = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_bool, 1);
    if (!has_next)
        return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = (int64_t) xi_lower_method_symbol(l, "hasNext") << 1;
    has_next->flags |= XI_FLAG_SIDE_EFFECT;
    has_next->line = line;
    xi_lower_insert_err_check(l, node, true);
    if (!l->cur_block)
        return;
    xi_block_set_if(l->cur_block, has_next, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, cond_blk);

    /* Body: var item = __iter.next(); <body> */
    l->cur_block = body_blk;
    XiValue *iter_body = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *next_val = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!next_val)
        return;
    next_val->args[0] = iter_body;
    next_val->aux = (void *) "next";
    next_val->aux_int = (int64_t) xi_lower_method_symbol(l, "next") << 1;
    next_val->flags |= XI_FLAG_SIDE_EFFECT;
    next_val->line = line;
    xi_lower_insert_err_check(l, node, true);
    if (!l->cur_block)
        return;

    struct XrType *item_type = xi_lower_node_type(l, node);
    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    xi_lower_braun_write(l, item_var, l->cur_block, next_val);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In Dispatcher ========== */

/* Whether the collection's static type is iterable via the fast
 * length + INDEX_GET path. Only Array, Slice and Set qualify: those
 * have integer indexable layouts that produce the loop variable's
 * canonical type directly. Map instead routes through the
 * iterator() / hasNext() / next() protocol, which lets `for (k in m)`
 * yield real keys, matching the analyzer's item-type inference.
 *
 * A `Range` value qualifies too: it carries no iterator() method, but both
 * backends answer len()/[i] on it lazily (VM: OP_LEN / OP_GETINDEX fast
 * paths; AOT: the XR_TAG_RANGE branches in xrt_len_value / xrt_index_get),
 * so the counted loop reads elements without materializing an array.
 *
 * The class_ref test keeps a user-declared `class Range` out of that path:
 * prelude names are ordinary identifiers, and the analyzer only grants the
 * builtin int-sequence domain to the prelude type (NULL class_ref). A user
 * Range reaches for-in through iterator() like any other class. */
static bool is_index_iterable_collection(XiLower *l, AstNode *coll_node) {
    struct XrType *t = xi_lower_node_type(l, coll_node);
    if (!t || t->kind == XR_KIND_UNKNOWN)
        return true; /* unknown: assume builtin for backward compat */
    return t->kind == XR_KIND_ARRAY || t->kind == XR_KIND_SLICE || t->kind == XR_KIND_SET ||
           (xr_type_is_named_class(t, "Range") && t->instance.class_ref == NULL);
}

static void lower_for_in_channel_loop(XiLower *l, AstNode *node, XiValue *coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    struct XrType *item_type = xi_lower_node_type(l, node);
    if (!item_type)
        item_type = l->type_any;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__for_ch_%d", sid);
    char *chan_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(chan_name != NULL, "arena alloc failed for chan_name");
    memcpy(chan_name, buf, strlen(buf) + 1);

    int chan_var = xi_lower_var_create(l, 0, chan_name, coll->type ? coll->type : l->type_any);
    xi_lower_braun_write(l, chan_var, l->cur_block, coll);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *body_chan = xi_lower_braun_read(l, chan_var, l->cur_block);
    XiValue *recv = xi_value_new(l->func, l->cur_block, XI_CHAN_RECV, item_type, 1);
    if (!recv)
        return;
    recv->args[0] = body_chan;
    recv->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
    recv->line = (uint32_t) node->line;

    XiValue *recv_status = lower_chan_recv_status(l, recv);
    if (!recv_status)
        return;
    xi_block_set_if(l->cur_block, recv_status, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, cond_blk);

    l->cur_block = body_blk;
    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    xi_lower_braun_write(l, item_var, l->cur_block, recv);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);
    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

XR_FUNC void xi_lower_for_in(XiLower *l, AstNode *node) {
    ForInStmtNode *s = &node->as.for_in_stmt;

    if (s->is_keyvalue) {
        lower_for_in_keyvalue(l, node);
        return;
    }

    if (s->collection->type == AST_RANGE) {
        RangeNode *rn = &s->collection->as.range;
        XiValue *start = xi_lower_expr(l, rn->start);
        if (!start || !l->cur_block)
            return;
        XiValue *end = xi_lower_expr(l, rn->end);
        if (!end || !l->cur_block)
            return;
        lower_for_in_loop(l, node, start, end, NULL, rn->inclusive_end);
        return;
    }

    if (s->domain_kind == XR_FOR_IN_DOMAIN_UNIT_ENUM_VALUES) {
        XaSymbol *enum_sym =
            s->enum_symbol_id ? xa_scope_lookup_by_id(l->analyzer->global_scope, s->enum_symbol_id)
                              : NULL;
        XaSymbolLinks *enum_links = enum_sym ? xa_analyzer_get_links(l->analyzer, enum_sym) : NULL;
        const char *enum_name =
            enum_links && enum_links->type && enum_links->type->kind == XR_KIND_ENUM
                ? enum_links->type->enum_type.enum_name
                : (enum_sym ? enum_sym->name : NULL);
        XiValue *enum_namespace =
            enum_sym && enum_name
                ? xi_lower_enum_namespace_value(l, enum_sym, enum_name, node->line)
                : xi_lower_expr(l, s->collection);
        if (!enum_namespace || !l->cur_block)
            return;
        XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        XiValue *limit =
            xi_const_int(l->func, l->cur_block, (int64_t) s->enum_variant_count, l->type_int);
        lower_for_in_loop(l, node, zero, limit, enum_namespace, false);
        return;
    }

    if (s->domain_kind == XR_FOR_IN_DOMAIN_ENUM_VARIANTS) {
        XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        XiValue *limit =
            xi_const_int(l->func, l->cur_block, (int64_t) s->enum_variant_count, l->type_int);
        lower_for_in_loop(l, node, zero, limit, NULL, false);
        return;
    }

    if (s->domain_kind == XR_FOR_IN_DOMAIN_ENUM_PAYLOADS) {
        XiValue *view = xi_lower_expr(l, s->collection);
        if (!view || !l->cur_block)
            return;
        XiValue *shift = xi_const_int(l->func, l->cur_block, 32, l->type_int);
        XiValue *limit = xi_binary(l->func, l->cur_block, XI_SHR, l->type_int, view, shift);
        XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        lower_for_in_loop(l, node, zero, limit, view, false);
        return;
    }

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block)
        return;

    struct XrType *coll_type = xi_lower_node_type(l, s->collection);
    if (coll_type && stmt_type_is_channel(coll_type)) {
        lower_for_in_channel_loop(l, node, coll);
        return;
    }

    /* Anything that isn't a fast index-iterable collection (Map,
     * tuple, struct, custom class) goes through the iterator() protocol.
     * The analyzer is responsible for rejecting collection types that
     * have no iterator() method (tuple / struct without one). */
    if (!is_index_iterable_collection(l, s->collection)) {
        lower_for_in_custom_iterator(l, node, coll);
        return;
    }

    /* Builtin collection iteration uses the same compiler-known len query as
     * source-level `len(coll)`.  Do not resurrect the removed `.length`
     * property as an internal lowering shortcut. */
    XiValue *len = xi_value_new(l->func, l->cur_block, XI_LEN, l->type_int, 1);
    if (!len)
        return;
    len->args[0] = coll;
    len->aux_int = 0;
    len->line = (uint32_t) node->line;

    XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    lower_for_in_loop(l, node, zero, len, coll, false);
}

/* ========== Try-Catch ========== */

/* Re-propagate an already-materialized error value (e.g. read by
 * XI_ERR_CATCH) through the value channel, respecting the enclosing
 * try scope:
 *   - inside an outer error-catch (try_depth > 0): XI_ERR_SET + jump to
 *     that catch block, so the error stays local to this function.
 *   - otherwise: XI_ERR_RETURN, returning the error from the function.
 * Consumes l->cur_block (sets it to the jump/return target state). */
XR_FUNC void xi_lower_reprop_error(XiLower *l, XiValue *val, AstNode *node) {
    if (!l->cur_block || !val)
        return;
    if (l->try_depth > 0) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_ERR_SET, l->type_unit, 1);
        if (set) {
            set->args[0] = val;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) node->line;
        }
        xi_lower_cleanup_run_to_depth(l, l->catch_cleanup_depths[l->try_depth - 1], node->line);
        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        xi_block_set_jump(l->cur_block, catch_blk);
        l->cur_block = NULL;
    } else {
        xi_lower_cleanup_run_to_depth(l, 0, node->line);
        if (!l->cur_block)
            return;
        XiValue *reprop = xi_value_new(l->func, l->cur_block, XI_ERR_RETURN, l->type_unit, 1);
        if (reprop) {
            reprop->args[0] = val;
            reprop->flags |= XI_FLAG_SIDE_EFFECT;
            reprop->line = (uint32_t) node->line;
        }
        l->cur_block->kind = XI_BLOCK_RETURN;
        l->cur_block->control = reprop;
        l->cur_block = NULL;
    }
}

/* Upper bound on catch clauses we partition on the stack.  Multi-catch
 * with more than this is pathological; clauses beyond it are ignored. */
#define XR_TRY_MAX_CATCH 32

static XrType *xi_lower_catch_clause_type(XiLower *l, XrCatchClause *cc) {
    if (!l || !cc || cc->is_panic || !cc->type)
        return l ? l->type_any : NULL;
    XrType *type = xr_tref_resolve_in_analyzer(l->analyzer, cc->type);
    return xi_lower_type_or_any(l, type, "catch clause type", cc->var_line);
}

static bool lower_catch_pattern_is_wildcard(AstNode *pattern) {
    return pattern && pattern->type == AST_PATTERN_WILDCARD;
}

static bool lower_catch_pattern_is_bare_type(const XrCatchClause *cc) {
    if (!cc || !cc->pattern || !cc->type || cc->type->kind != XR_TREF_NAMED || !cc->type->name)
        return false;
    AstNode *pattern = cc->pattern;
    if (pattern->type != AST_PATTERN_LITERAL || !pattern->as.pattern_literal.value)
        return false;
    AstNode *value = pattern->as.pattern_literal.value;
    return value->type == AST_VARIABLE && value->as.variable.name &&
           strcmp(value->as.variable.name, cc->type->name) == 0;
}

static bool lower_catch_clause_has_pattern_test(const XrCatchClause *cc) {
    return cc && cc->pattern && !lower_catch_pattern_is_wildcard(cc->pattern) &&
           !lower_catch_pattern_is_bare_type(cc);
}

static XiValue *lower_catch_narrow_value(XiLower *l, XiValue *catch_op, XrCatchClause *cc,
                                         XrType *catch_type) {
    if (!l || !catch_op || !catch_type || !cc || !cc->type)
        return catch_op;
    if (catch_op->type && xr_type_equals(catch_op->type, catch_type))
        return catch_op;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, catch_type, 1);
    if (!v)
        return catch_op;
    v->args[0] = catch_op;
    v->aux_int = ((int64_t) (uint32_t) -1 << 1);
    v->aux = (void *) arena_strdup(l->func, cc->type->name ? cc->type->name : "unknown");
    v->conversion.kind = XR_CONVERSION_DYNAMIC_CHECKED;
    v->conversion.source_scalar_rep = XR_SCALAR_REP_NONE;
    v->conversion.target_scalar_rep = XR_SCALAR_REP_NONE;
    v->conversion.is_implicit = false;
    v->line = (uint32_t) (cc->var_line > 0 ? cc->var_line : 0);
    return v;
}

static void lower_catch_bind_clause(XiLower *l, XrCatchClause *cc, XiValue *value) {
    if (!l || !cc || !value)
        return;
    if (cc->var_name) {
        int var_id = xi_lower_var_create(l, cc->symbol_id, cc->var_name,
                                         value->type ? value->type : l->type_any);
        xi_lower_braun_write(l, var_id, l->cur_block, value);
    }
    if (lower_catch_clause_has_pattern_test(cc))
        lower_pattern_bindings(l, value, cc->pattern);
}

/* Lower the error catch block: XI_ERR_CATCH binds the pending error,
 * then the error catch clauses run (single or is-T chain).  All control
 * flow is the value-return error channel — no handler stack. */
static void lower_error_catch_clauses(XiLower *l, XrCatchClause **errc, int errn, AstNode *node,
                                      XiBlock *normal_target) {
    XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_ERR_CATCH, l->type_any, 0);
    if (catch_op) {
        catch_op->flags |= XI_FLAG_SIDE_EFFECT;
        catch_op->line = (errn > 0 && errc[0]->var_line > 0) ? (uint32_t) errc[0]->var_line
                                                             : (uint32_t) node->line;
    }

    for (int ci = 0; ci < errn; ci++) {
        XrCatchClause *cc = errc[ci];
        if (!cc || !l->cur_block)
            break;
        bool has_type = cc->type != NULL;
        bool has_pattern_test = lower_catch_clause_has_pattern_test(cc);
        XiBlock *next_blk = NULL;

        if (has_type) {
            XiValue *is_val = xi_lower_is_test(l, catch_op, cc->type, cc->var_line, 0);
            if (!is_val) {
                /* Rejected target type; the clause dispatch cannot be built. */
                l->had_error = true;
                return;
            }
            XiBlock *type_blk = xi_block_new(l->func);
            next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, is_val, type_blk, next_blk);
            xi_lower_braun_seal(l, type_blk);
            l->cur_block = type_blk;
        }

        XrType *catch_type = has_type ? xi_lower_catch_clause_type(l, cc) : NULL;
        XiValue *match_value =
            has_type ? lower_catch_narrow_value(l, catch_op, cc, catch_type) : catch_op;

        if (has_pattern_test) {
            XiValue *pattern_test = xi_lower_pattern_test(l, match_value, cc->pattern);
            if (!pattern_test) {
                /* Rejected pattern; the clause dispatch cannot be built. */
                l->had_error = true;
                return;
            }
            XiBlock *body_blk = xi_block_new(l->func);
            if (!next_blk)
                next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, pattern_test, body_blk, next_blk);
            xi_lower_braun_seal(l, body_blk);
            l->cur_block = body_blk;
        }

        lower_catch_bind_clause(l, cc, match_value);
        xi_lower_stmt(l, cc->body);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, normal_target);

        if (!next_blk) {
            l->cur_block = NULL;
            break;
        }
        xi_lower_braun_seal(l, next_blk);
        l->cur_block = next_blk;
    }

    if (l->cur_block)
        xi_lower_reprop_error(l, catch_op, node);
}

/* try-catch (with optional finally).  error and panic are two strictly
 * separate channels:
 *
 *   - `catch (e)` / `catch (e: T)`  → ERROR channel (user `throw <enum>`).
 *     Pure value-return: pending_error + CFG branches.  No handler stack.
 *
 *   - `catch panic (p)`             → PANIC channel (div-zero, OOB, expr!,
 *     assert, …).  Uses XI_TRY/OP_TRY handler stack + unwind.  Only this
 *     clause observes runtime faults.
 *
 * XI_TRY is emitted iff a `catch panic` clause is present. */
static void lower_try_catch_impl(XiLower *l, TryCatchNode *tc, AstNode *node) {
    /* Partition catch clauses: error clauses vs. the (optional) panic clause. */
    XrCatchClause *errc[XR_TRY_MAX_CATCH];
    int errn = 0;
    XrCatchClause *panic_clause = NULL;
    for (int i = 0; i < tc->catch_count; i++) {
        XrCatchClause *cc = tc->catch_clauses[i];
        if (!cc)
            continue;
        if (cc->is_panic)
            panic_clause = cc;
        else if (errn < XR_TRY_MAX_CATCH)
            errc[errn++] = cc;
    }
    bool has_err = errn > 0;
    bool has_panic = panic_clause != NULL;

    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = has_err ? xi_block_new(l->func) : NULL;
    XiBlock *panic_blk = has_panic ? xi_block_new(l->func) : NULL;
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *normal_target = merge;

    /* Panic handler: register OP_TRY pointing at panic_blk.  This is the
     * VM's mechanism for synchronous runtime faults (the only thing that
     * uses the handler stack now). */
    XiValue *try_op = NULL;
    if (has_panic) {
        try_op = xi_value_new(l->func, l->cur_block, XI_TRY, l->type_unit, 0);
        if (try_op) {
            try_op->aux = (void *) panic_blk;
            try_op->aux_int = l->cleanup_body_depth > 0 ? XI_TRY_AUX_CLEANUP_LOCAL_HANDLER : -1;
            try_op->flags |= XI_FLAG_SIDE_EFFECT;
            try_op->line = (uint32_t) node->line;
        }
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    /* Error catch scope: a `throw <enum>` or fallible call inside the body
     * branches to catch_blk via the value channel (see lower_throw /
     * xi_lower_insert_err_check, which consult try_depth/catch_targets). */
    if (has_err) {
        l->catch_targets[l->try_depth] = catch_blk;
        l->catch_cleanup_depths[l->try_depth] = l->cleanup_scope_depth;
        l->try_depth++;
    }
    l->cur_block = try_blk;
    l->dead_after_throw = false;
    xi_lower_stmt(l, tc->try_body);
    if (has_err)
        l->try_depth--;

    XiBlock *try_exit_blk = l->cur_block;

    /* Normal path: pop panic handler (if any) and go to merge. */
    if (l->cur_block) {
        if (has_panic) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->aux = (void *) try_op;
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
        }
        xi_block_set_jump(l->cur_block, normal_target);
    }

    /* ---- Error catch block (value channel) ---- */
    if (has_err) {
        /* The body reaches catch_blk via CFG edges (ERR_SET+jump or
         * ERR_HAS+IF).  If none exist, add try exit as predecessor so
         * Braun SSA still sees the try body's definitions. */
        XiBlock *catch_pred = try_exit_blk ? try_exit_blk : try_blk;
        if (catch_blk->npreds == 0)
            xi_block_add_pred(catch_blk, catch_pred);
        xi_lower_braun_seal(l, catch_blk);
        l->cur_block = catch_blk;
        l->dead_after_throw = false;

        /* Leaving the try scope on the error path: pop the panic handler. */
        if (has_panic) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->aux = (void *) try_op;
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
        }

        lower_error_catch_clauses(l, errc, errn, node, normal_target);

        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* ---- Panic catch block (unwind channel) ---- */
    if (has_panic) {
        /* panic_blk is reached only via the implicit unwind edge (OP_TRY
         * handler), invisible to the SSA builder.  Add try entry as pred. */
        if (panic_blk->npreds == 0)
            xi_block_add_pred(panic_blk, try_blk);
        xi_lower_braun_seal(l, panic_blk);
        l->cur_block = panic_blk;
        l->dead_after_throw = false;

        XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_CATCH, l->type_any, 0);
        if (catch_op) {
            catch_op->aux = (void *) try_op;
            catch_op->flags |= XI_FLAG_SIDE_EFFECT;
            catch_op->line = (uint32_t) panic_clause->var_line;
        }
        if (panic_clause->var_name && catch_op) {
            int var_id = xi_lower_var_create(l, panic_clause->symbol_id, panic_clause->var_name,
                                             l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
        }
        xi_lower_stmt(l, panic_clause->body);

        /* Pop the handler now that the panic is handled. */
        if (l->cur_block) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->aux = (void *) try_op;
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
            xi_block_set_jump(l->cur_block, merge);
        }
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    l->dead_after_throw = false;
}

XR_FUNC void xi_lower_try_catch(XiLower *l, AstNode *node) {
    lower_try_catch_impl(l, &node->as.try_catch, node);
}

/* ========== Defer / Yield (from xi_lower_expr.c) ========== */

static void stmt_mark_storage_allocs_in_range(XiBlock *block, uint32_t begin, uint8_t storage_mode);

typedef struct XiCleanupPlaceScan {
    XiLower *lower;
    XaAstVisitor visitor;
    AstNode *root;
    XaScope *body_scope;
} XiCleanupPlaceScan;

typedef struct XiCleanupPlacePlanScan {
    XiLower *lower;
    XaAstVisitor visitor;
    AstNode *root;
} XiCleanupPlacePlanScan;

static bool cleanup_scope_is_within(XaScope *scope, XaScope *ancestor) {
    for (XaScope *current = scope; current; current = current->parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

static bool lower_cleanup_place_symbol_append(XiLower *l, uint32_t symbol_id) {
    if (!l || symbol_id == 0)
        return true;
    for (int i = 0; i < l->cleanup_place_symbol_count; i++) {
        if (l->cleanup_place_symbols[i] == symbol_id)
            return true;
    }
    if (l->cleanup_place_symbol_count >= l->cleanup_place_symbol_cap) {
        int capacity = l->cleanup_place_symbol_cap ? l->cleanup_place_symbol_cap * 2 : 8;
        uint32_t *grown =
            (uint32_t *) xr_realloc(l->cleanup_place_symbols, (size_t) capacity * sizeof(uint32_t));
        if (!grown)
            return false;
        l->cleanup_place_symbols = grown;
        l->cleanup_place_symbol_cap = capacity;
    }
    l->cleanup_place_symbols[l->cleanup_place_symbol_count++] = symbol_id;
    return true;
}

XR_FUNC bool xi_lower_cleanup_symbol_needs_place(const XiLower *l, uint32_t symbol_id) {
    if (!l || symbol_id == 0)
        return false;
    for (int i = 0; i < l->cleanup_place_symbol_count; i++) {
        if (l->cleanup_place_symbols[i] == symbol_id)
            return true;
    }
    return false;
}

XR_FUNC bool xi_lower_cleanup_bind_place(XiLower *l, int var_id, XiValue *initial_value, int line) {
    if (!l || var_id < 0 || var_id >= l->var_count || !initial_value)
        return false;
    if (l->vars[var_id].call_place ||
        !xi_lower_cleanup_symbol_needs_place(l, l->vars[var_id].symbol_id))
        return true;
    if (l->is_program && l->shared_map && l->shared_map[var_id] >= 0)
        return true;
    XiValue *place =
        xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR,
                     l->vars[var_id].type ? l->vars[var_id].type : initial_value->type, 1);
    if (!place)
        return false;
    place->args[0] = initial_value;
    place->aux_int |= XI_LOCAL_ADDR_AUX_CLEANUP_LIVE;
    place->line = (uint32_t) line;
    l->vars[var_id].call_place = place;
    l->vars[var_id].place_mode = XR_PARAM_REF;
    return true;
}

static void lower_cleanup_place_plan_ref(AstNode *node, void *user_data) {
    XiCleanupPlaceScan *scan = (XiCleanupPlaceScan *) user_data;
    if (!scan || !node)
        return;
    if (node != scan->root && (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
                               node->type == AST_METHOD_DECL)) {
        scan->visitor.skip_children = true;
        return;
    }
    uint32_t symbol_id = 0;
    switch (node->type) {
        case AST_VARIABLE:
            symbol_id = node->as.variable.symbol_id;
            break;
        case AST_ASSIGNMENT:
            symbol_id = node->as.assignment.symbol_id;
            break;
        case AST_COMPOUND_ASSIGNMENT:
            symbol_id = node->as.compound_assignment.symbol_id;
            break;
        case AST_INC:
            symbol_id = node->as.inc.symbol_id;
            break;
        case AST_DEC:
            symbol_id = node->as.dec.symbol_id;
            break;
        default:
            return;
    }
    XaSymbol *symbol =
        symbol_id ? xa_scope_lookup_by_id(scan->lower->analyzer->global_scope, symbol_id) : NULL;
    if (!symbol || cleanup_scope_is_within(symbol->scope, scan->body_scope))
        return;
    if (!lower_cleanup_place_symbol_append(scan->lower, symbol_id))
        scan->lower->had_error = true;
}

static void lower_cleanup_place_plan_pre(AstNode *node, void *user_data) {
    XiCleanupPlacePlanScan *scan = (XiCleanupPlacePlanScan *) user_data;
    if (!scan || !node)
        return;
    if (node != scan->root && (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
                               node->type == AST_METHOD_DECL)) {
        scan->visitor.skip_children = true;
        return;
    }
    if (node->type != AST_DEFER_STMT || !node->as.defer_stmt.body)
        return;
    XiCleanupPlaceScan refs;
    memset(&refs, 0, sizeof(refs));
    refs.lower = scan->lower;
    refs.root = node->as.defer_stmt.body;
    refs.body_scope = xa_scope_find_by_node(scan->lower->analyzer->global_scope, refs.root);
    refs.visitor.visit_pre = lower_cleanup_place_plan_ref;
    refs.visitor.user_ctx = &refs;
    xa_ast_visit(refs.root, &refs.visitor);
}

XR_FUNC void xi_lower_prepare_cleanup_places(XiLower *l, AstNode *root) {
    if (!l || !root || !l->analyzer)
        return;
    XiCleanupPlacePlanScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.lower = l;
    scan.root = root;
    scan.visitor.visit_pre = lower_cleanup_place_plan_pre;
    scan.visitor.user_ctx = &scan;
    xa_ast_visit(root, &scan.visitor);
}

static void lower_cleanup_promote_symbol(XiCleanupPlaceScan *scan, uint32_t symbol_id,
                                         const char *name, int line) {
    if (!scan || !scan->lower || !scan->lower->cur_block || symbol_id == 0)
        return;
    XiLower *l = scan->lower;
    XaSymbol *symbol = xa_scope_lookup_by_id(l->analyzer->global_scope, symbol_id);
    if (!symbol || cleanup_scope_is_within(symbol->scope, scan->body_scope))
        return;
    int var_id = xi_lower_var_find(l, symbol_id, name);
    if (var_id < 0 || var_id >= l->var_count || l->vars[var_id].call_place)
        return;
    if (l->is_program && l->shared_map && l->shared_map[var_id] >= 0)
        return;
    XiValue *current = xi_lower_braun_read(l, var_id, l->cur_block);
    if (!current)
        return;
    XiValue *place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR,
                                  l->vars[var_id].type ? l->vars[var_id].type : current->type, 1);
    if (!place) {
        l->had_error = true;
        return;
    }
    place->args[0] = current;
    place->aux_int |= XI_LOCAL_ADDR_AUX_CLEANUP_LIVE;
    place->line = (uint32_t) line;
    l->vars[var_id].call_place = place;
    l->vars[var_id].place_mode = XR_PARAM_REF;
}

static void lower_cleanup_place_scan_pre(AstNode *node, void *user_data) {
    XiCleanupPlaceScan *scan = (XiCleanupPlaceScan *) user_data;
    if (!scan || !node)
        return;
    if (node != scan->root && (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
                               node->type == AST_METHOD_DECL)) {
        scan->visitor.skip_children = true;
        return;
    }
    switch (node->type) {
        case AST_VARIABLE:
            lower_cleanup_promote_symbol(scan, node->as.variable.symbol_id, node->as.variable.name,
                                         node->line);
            break;
        case AST_ASSIGNMENT:
            lower_cleanup_promote_symbol(scan, node->as.assignment.symbol_id,
                                         node->as.assignment.name, node->line);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            lower_cleanup_promote_symbol(scan, node->as.compound_assignment.symbol_id,
                                         node->as.compound_assignment.name, node->line);
            break;
        case AST_INC:
            lower_cleanup_promote_symbol(scan, node->as.inc.symbol_id, node->as.inc.name,
                                         node->line);
            break;
        case AST_DEC:
            lower_cleanup_promote_symbol(scan, node->as.dec.symbol_id, node->as.dec.name,
                                         node->line);
            break;
        default:
            return;
    }
}

static void lower_cleanup_promote_places(XiLower *l, AstNode *body) {
    if (!l || !body || !l->analyzer)
        return;
    XiCleanupPlaceScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.lower = l;
    scan.root = body;
    scan.body_scope = xa_scope_find_by_node(l->analyzer->global_scope, body);
    scan.visitor.visit_pre = lower_cleanup_place_scan_pre;
    scan.visitor.user_ctx = &scan;
    xa_ast_visit(body, &scan.visitor);
}

static void lower_defer(XiLower *l, AstNode *node) {
    DeferStmtNode *d = &node->as.defer_stmt;
    AstNode *body = d->body;
    if (!body || body->type != AST_BLOCK || !l->cur_block)
        return;
    if (l->cleanup_scope_depth <= 0)
        xi_lower_cleanup_scope_push(l);
    XiCleanupScope *scope = &l->cleanup_scopes[l->cleanup_scope_depth - 1];

    /* Every external binding read by the cleanup uses stable same-frame
     * storage. This makes normal and panic copies of the cleanup CFG observe
     * the latest value without creating an upvalue or cell. */
    lower_cleanup_promote_places(l, body);

    /* A new registration closes the preceding panic interval and opens one
     * whose catch edge owns the larger, statically known frontier. */
    if (scope->active_try)
        lower_cleanup_emit_end_try(l, scope, node->line);
    if (!lower_cleanup_scope_append_block(scope, node)) {
        l->had_error = true;
        return;
    }
    if (!lower_cleanup_open_panic_interval(l, scope, node->line))
        l->had_error = true;
}

static void lower_yield_stmt(XiLower *l, AstNode *node) {
    /* `yield expr` produces a generator value. The enclosing function is a
     * generator (suspendable coroutine); XI_GEN_YIELD suspends and hands the
     * value to the driving iterator. (Cooperative scheduling is Coro.yield().) */
    AstNode *value_node = node ? node->as.yield_stmt.value : NULL;
    XiBlock *allocation_block = l->cur_block;
    uint32_t allocation_begin = allocation_block ? allocation_block->nvalues : 0;
    XiValue *value = value_node ? xi_lower_expr(l, value_node) : NULL;
    if (!value)
        value = xi_const_null(l->func, l->cur_block, l->type_null);
    if (allocation_block == l->cur_block)
        stmt_mark_storage_allocs_in_range(allocation_block, allocation_begin,
                                          XR_OBJ_STORAGE_TRANSFER);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GEN_YIELD, l->type_unit, 1);
    if (v) {
        v->args[0] = value;
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
        v->line = node ? (uint32_t) node->line : 0;
    }
}

/* ========== Destructuring (from xi_lower_expr.c) ========== */

/*
 * Bind destructure pattern elements to extracted values from 'src'.
 * Array patterns: INDEX_GET by position.
 * Object patterns: indexed Json field read when a complete static field table
 * is known, otherwise string-key INDEX_GET fallback.
 * Identifier patterns: bind directly.
 */
static void lower_destructure_bind(XiLower *l, XrDestructurePattern *pat, XiValue *src,
                                   uint32_t source_span_id) {
    if (!pat || !src || !l->cur_block)
        return;

    switch (pat->type) {
        case PATTERN_ARRAY: {
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem)
                    continue;
                XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
                XiValue *val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, l->type_any, 2);
                if (val) {
                    val->args[0] = src;
                    val->args[1] = idx;
                }
                lower_destructure_bind(l, elem, val, source_span_id);
            }
            break;
        }
        case PATTERN_TUPLE: {
            /* Tuples are heterogeneous and immutable: each element comes
             * from a fixed compile-time position, read via XI_TUPLE_GET.
             * The analyzer has bounds-checked arity at the decl site, so
             * we trust pat->as.array.element_count here. */
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem)
                    continue;
                struct XrType *element_type = tuple_elem_type(l, src->type, i);
                XiValue *val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, element_type, 1);
                if (val) {
                    val->args[0] = src;
                    val->aux_int = i;
                }
                lower_destructure_bind(l, elem, val, source_span_id);
            }
            break;
        }
        case PATTERN_OBJECT: {
            int n = pat->as.object.field_count;
            for (int i = 0; i < n; i++) {
                char *fname = pat->as.object.field_names[i];
                XrDestructurePattern *sub = pat->as.object.patterns[i];
                if (!fname)
                    continue;
                int fidx = stmt_json_field_index(src->type, fname);
                XiValue *val = NULL;
                if (fidx >= 0) {
                    struct XrType *field_type =
                        src->type->object.field_types ? src->type->object.field_types[fidx] : NULL;
                    val = xi_value_new(l->func, l->cur_block, XI_OBJECT_GET_F,
                                       field_type ? field_type : l->type_any, 1);
                    if (val) {
                        val->args[0] = src;
                        val->aux_int = fidx;
                        val->line = source_span_id;
                        xi_lower_bind_object_access_id(l, val, fname, source_span_id,
                                                       XG_OBJECT_ACCESS_DESTRUCTURE);
                    }
                } else {
                    XiValue *key = xi_const_str(l->func, l->cur_block, fname, l->type_string);
                    val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, l->type_any, 2);
                    if (val) {
                        val->args[0] = src;
                        val->args[1] = key;
                    }
                }
                lower_destructure_bind(l, sub, val, source_span_id);
            }
            break;
        }
        case PATTERN_IDENTIFIER: {
            const char *name = pat->as.identifier.name;
            if (!name)
                break;
            uint32_t sid = pat->as.identifier.symbol_id;
            /* Resolution order mirrors lower_assignment: local var
             * (with shared-slot follow-up if program-level), then
             * shared from an enclosing scope, then upvalue. The
             * destructure-decl form is handled by the create-write
             * fast path below; only the assign form needs the wider
             * search because the identifier may resolve outward. */
            int var_id = xi_lower_var_find(l, sid, name);
            if (var_id >= 0) {
                xi_lower_braun_write(l, var_id, l->cur_block, src);
                if (l->is_program && l->shared_map[var_id] >= 0) {
                    XiTopBinding b;
                    b.slot = l->shared_map[var_id];
                    b.name = l->vars[var_id].name;
                    b.type = l->vars[var_id].type;
                    xi_lower_emit_top_store(l, b, src);
                }
                break;
            }
            XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
            if (xi_top_binding_valid(tb)) {
                xi_lower_emit_top_store(l, tb, src);
                break;
            }
            int upval_idx = xi_lower_resolve_upvalue(l, sid, name, NULL);
            if (upval_idx >= 0) {
                XiValue *store =
                    xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL, l->type_unit, 1);
                if (store) {
                    store->args[0] = src;
                    store->aux_int = upval_idx;
                    store->flags |= XI_FLAG_SIDE_EFFECT;
                }
                break;
            }
            /* Fall through: declaration-style binding (create fresh
             * local). Reached for destructure-decl PATTERN_IDENTIFIER
             * because the analyzer has not pre-bound the symbol. */
            struct XrType *binding_type = src->type ? src->type : l->type_any;
            int new_var = xi_lower_var_create(l, sid, name, binding_type);
            xi_lower_braun_write(l, new_var, l->cur_block, src);
            break;
        }
        case PATTERN_SKIP:
            break;
        default:
            XR_CHECK(false, "xi_lower: invalid destructure pattern");
    }
}

/* Destructure declaration: var [a, b] = expr or var {x, y} = expr */
static void lower_destructure_decl(XiLower *l, AstNode *node) {
    DestructureDeclNode *dd = &node->as.destructure_decl;
    XiValue *init = xi_lower_expr(l, dd->initializer);
    if (!init || !dd->pattern)
        return;
    lower_destructure_bind(l, dd->pattern, init, (uint32_t) node->line);
}

/* Destructure assignment: [a, b] = [b, a] or (a, b) = (b, a) */
static void lower_destructure_assign(XiLower *l, AstNode *node) {
    DestructureAssignNode *da = &node->as.destructure_assign;
    XiValue *rhs = xi_lower_expr(l, da->value);
    if (!rhs || !da->pattern)
        return;
    lower_destructure_bind(l, da->pattern, rhs, (uint32_t) node->line);
}

/* ========== Basic Statement Lowering (from xi_lower_expr.c) ========== */

/* ========== Statement Lowering ========== */

static void lower_mark_decl_captured_by_child(XiLower *l, int var_id, const char *name,
                                              XiValue *value) {
    if (!l || var_id < 0 || var_id >= l->var_count || !name)
        return;
    for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
        XiFunc *child = l->func->children[ci_fn];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            XiCapture *cap = &child->captures[ci];
            if (cap->source == XI_CAPTURE_SRC_REG && cap->name && strcmp(cap->name, name) == 0) {
                cap->needs_cell = true;
                l->vars[var_id].captured_by_child = true;
                if (value)
                    value->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
    }
}

static bool stmt_mark_value_storage_alloc(XiValue *v, uint8_t storage_mode) {
    return xi_lower_mark_storage_allocation(v, storage_mode);
}

static void stmt_mark_storage_allocs_in_range(XiBlock *block, uint32_t begin,
                                              uint8_t storage_mode) {
    if (!block || begin > block->nvalues)
        return;
    for (uint32_t i = begin; i < block->nvalues; i++)
        stmt_mark_value_storage_alloc(block->values[i], storage_mode);
}

static void stmt_apply_canonical_allocation_plan(XiLower *l, AstNode *decl, XiBlock *block,
                                                 uint32_t begin) {
    if (!l || !l->analyzer || !decl || decl->as.var_decl.symbol_id == 0)
        return;
    XaSymbol *symbol =
        xa_scope_lookup_by_id(l->analyzer->global_scope, decl->as.var_decl.symbol_id);
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(l->analyzer, symbol) : NULL;
    if (!links || !links->allocation_plan.complete)
        return;
    if (links->allocation_plan.domain == XR_STORAGE_TRANSFERABLE)
        stmt_mark_storage_allocs_in_range(block, begin, XR_OBJ_STORAGE_TRANSFER);
    else if (links->allocation_plan.domain == XR_STORAGE_CONST_SHARED ||
             links->allocation_plan.domain == XR_STORAGE_SYNC_SHARED)
        stmt_mark_storage_allocs_in_range(block, begin, XR_OBJ_STORAGE_SHARED);
}

static XiFunc *stmt_static_function_value_target(XiValue *value) {
    while (value &&
           (value->op == XI_BOX || value->op == XI_UNBOX || xi_copy_is_identity_alias(value) ||
            xi_op_is_identity_forward(value->op)) &&
           value->nargs >= 1) {
        value = value->args[0];
    }
    if (!value)
        return NULL;
    if ((value->op == XI_CLOSURE_NEW ||
         (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW)) &&
        value->aux) {
        return (XiFunc *) value->aux;
    }
    return NULL;
}

static void stmt_record_shared_function_value(XiLower *l, int slot, XiValue *value) {
    if (!l || slot < 0)
        return;
    XiFunc *target = stmt_static_function_value_target(value);
    if (!target)
        return;
    if (slot < l->var_cap)
        l->shared_slot_funcs[slot] = target;
    if (l->func && l->func->shared_slot_funcs && slot < (int) l->func->shared_slot_func_count)
        l->func->shared_slot_funcs[slot] = target;
}

static bool stmt_freestanding_static_aggregate_is_erased(XiLower *l, AstNode *node, int var_id) {
    if (!l || !l->is_program || !l->analyzer || !xa_analyzer_is_freestanding(l->analyzer) ||
        !node || var_id < 0 || !l->shared_map)
        return false;
    int slot = l->shared_map[var_id];
    if (slot < 0 || !l->func)
        return false;
    if (node->type == AST_CONST_DECL) {
        if (!l->func->shared_const_literals || slot >= (int) l->func->shared_const_literal_count)
            return false;
        return l->func->shared_const_literals[slot].kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE;
    }
    if (node->type == AST_VAR_DECL) {
        if (!l->func->shared_init_literals || slot >= (int) l->func->shared_init_literal_count)
            return false;
        return l->func->shared_init_literals[slot].kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE;
    }
    return false;
}

static void stmt_write_decl_value(XiLower *l, int var_id, XiValue *init_val) {
    xi_lower_braun_write(l, var_id, l->cur_block, init_val);
    if (!l->is_program || l->shared_map[var_id] < 0)
        return;
    XiTopBinding binding = {
        .slot = l->shared_map[var_id],
        .name = l->vars[var_id].name,
        .type = l->vars[var_id].type,
    };
    stmt_record_shared_function_value(l, binding.slot, init_val);
    xi_lower_emit_top_store(l, binding, init_val);
}

static void lower_var_decl(XiLower *l, AstNode *node) {
    const char *name = node->as.var_decl.name;
    uint32_t sid = node->as.var_decl.symbol_id;
    struct XrType *type = xi_lower_node_type(l, node);

    int var_id = xi_lower_var_create(l, sid, name, type);
    if (stmt_freestanding_static_aggregate_is_erased(l, node, var_id)) {
        XiValue *placeholder = xi_const_null(l->func, l->cur_block, l->type_null);
        if (placeholder)
            xi_lower_braun_write(l, var_id, l->cur_block, placeholder);
        return;
    }

    XiBlock *init_block = l->cur_block;
    uint32_t init_begin = init_block ? init_block->nvalues : 0;

    XiValue *init_val;
    if (node->as.var_decl.initializer) {
        /* Self-referential initializers must bind to this declaration, not a
         * loop-carried value from a previous iteration. If a child closure
         * captures this null placeholder, resolve_upvalue marks it as a cell
         * capture and the real initializer below is written through that cell.
         */
        XiValue *placeholder = xi_const_null(l->func, l->cur_block, l->type_null);
        if (placeholder)
            xi_lower_braun_write(l, var_id, l->cur_block, placeholder);

        init_val = xi_lower_expr(l, node->as.var_decl.initializer);
        if (!init_val)
            return;
        int init_line = node->as.var_decl.initializer->line > 0
                            ? node->as.var_decl.initializer->line
                            : node->line;
        stmt_set_missing_line(init_val, init_line);
        lower_mark_decl_captured_by_child(l, var_id, name, init_val);
        init_val = xi_lower_apply_numeric_conversion_witness(l, node->as.var_decl.initializer,
                                                             init_val, type);
        if (!init_val)
            return;
        init_val = xi_lower_checktype_for_type(l, node, init_val, type);
        init_val = stmt_narrow_for_target_type(l, node, init_val, type);
    } else {
        /* Zero-value initialization for typed variables without initializer.
         * Nullable types (T?) default to null. Non-nullable primitives:
         * int→0, float→0.0, bool→false; default-initializable structs
         * allocate a zero/default-filled struct value. */
        init_val = stmt_default_struct_value(l, type, node->line);
        if (init_val) {
            /* Already built. */
        } else if (type && !type->is_nullable && type->kind == XR_KIND_INT)
            init_val = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        else if (type && !type->is_nullable && type->kind == XR_KIND_FLOAT)
            init_val = xi_const_float(l->func, l->cur_block, 0.0, l->type_float);
        else if (type && !type->is_nullable && type->kind == XR_KIND_BOOL)
            init_val = xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        else
            init_val = xi_const_null(l->func, l->cur_block, l->type_null);
        init_val = stmt_narrow_for_target_type(l, node, init_val, type);
    }
    stmt_set_missing_line(init_val, node->line);

    /* A const declaration seals the initializer's capability at the binding
     * boundary. Preserve that fact in SSA instead of leaving variable reads
     * typed as the mutable initializer expression. */
    if (node->type == AST_CONST_DECL && type && xr_type_is_const(type) &&
        (!init_val->type || !xr_type_is_const(init_val->type))) {
        XiValue *sealed = xi_value_new(l->func, l->cur_block, XI_COPY, type, 1);
        if (!sealed)
            return;
        sealed->args[0] = init_val;
        sealed->aux_int = XI_COPY_KIND_IDENTITY;
        sealed->line = (uint32_t) node->line;
        init_val = sealed;
    }

    /* Propagate reified generic elem_tid when there is an explicit type
     * annotation on a container literal (e.g. var a: Array<int> = [1,2]).
     * Only the annotation distinguishes typed from untyped containers. */
    if (node->as.var_decl.type_annotation && type) {
        bool is_array_factory = init_val->op == XI_CALL_BUILTIN && init_val->aux &&
                                strcmp((const char *) init_val->aux, "array_with_capacity") == 0;
        if ((init_val->op == XI_ARRAY_NEW || is_array_factory) &&
            (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_SLICE(type)) && type->container.element_type) {
            uint8_t tid = xr_type_to_tid(type->container.element_type);
            init_val->type = type;
            init_val->aux_int = (int64_t) (tid << 2);
        } else if (init_val->op == XI_SET_NEW && type->kind == XR_KIND_SET &&
                   type->container.element_type) {
            uint8_t tid = xr_type_to_tid(type->container.element_type);
            uint8_t flags = (uint8_t) (init_val->aux_int & 0x04);
            init_val->aux_int = (int64_t) (((tid & 0x1F) << 3) | flags);
        } else if (init_val->op == XI_CHAN_NEW && type->kind == XR_KIND_CHANNEL &&
                   type->container.element_type) {
            init_val->aux_int = xr_type_to_tid(type->container.element_type);
        } else if (init_val->op == XI_MAP_NEW && XR_TYPE_IS_MAP(type)) {
            uint8_t flags = (uint8_t) (init_val->aux_int & 0x04);
            uint8_t value_tid = 0, key_kind = 0;
            if (type->map.value_type)
                value_tid = xr_type_to_tid(type->map.value_type);
            if (type->map.key_type) {
                uint8_t ktid = xr_type_to_tid(type->map.key_type);
                if (ktid == XR_TID_STRING)
                    key_kind = 1;
                else if (ktid == XR_TID_INT)
                    key_kind = 2;
            }
            init_val->aux_int = (int64_t) ((key_kind << 8) | ((value_tid & 0x1F) << 3) | flags);
        }
    }
    stmt_apply_canonical_allocation_plan(l, node, init_block, init_begin);
    /* When the initializer comes from a different variable, insert an
     * explicit copy so the new variable gets its own SSA value.  Without
     * this, both variables map to the same physical register and
     * loop-carried updates to the source corrupt the snapshot. */
    bool needs_copy =
        (xi_var_id_is_valid(init_val->var_id) && init_val->var_id != (XiVarId) var_id);
    /* Value types (structs and fixed arrays) always need a value copy regardless
     * of var_id — the source could be a shared variable, upvalue, or
     * function return whose identity must not leak into the new binding. */
    bool value_clone_copy =
        (stmt_type_needs_value_clone(l, type) || stmt_type_needs_value_clone(l, init_val->type)) &&
        !stmt_value_is_fresh_value_struct(init_val);
    if (!needs_copy && value_clone_copy) {
        needs_copy = true;
    }
    if (needs_copy) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, init_val->type, 1);
        if (copy) {
            copy->args[0] = init_val;
            copy->line = (uint32_t) node->line;
            if (value_clone_copy)
                stmt_mark_value_clone_copy(copy);
            init_val = copy;
        }
    }
    if (!stmt_bind_stable_value_place(l, node, var_id, init_val))
        return;
    stmt_write_decl_value(l, var_id, init_val);
}

static void lower_print(XiLower *l, AstNode *node) {
    PrintNode *p = &node->as.print_stmt;
    int nargs = (int) p->expr_count;
    if (nargs < 0 || nargs > (int) UINT16_MAX) {
        fprintf(stderr, "[LOWER] print argument count exceeds %u at line %d\n",
                (unsigned) UINT16_MAX, (int) node->line);
        l->had_error = true;
        return;
    }

    XiValue *stack_args[32];
    XiValue **arg_vals = stack_args;
    if (nargs > 32) {
        arg_vals = (XiValue **) xi_func_arena_alloc(
            l->func, (uint32_t) ((size_t) nargs * sizeof(XiValue *)));
        if (!arg_vals)
            return;
    }
    for (int i = 0; i < nargs; i++) {
        arg_vals[i] = xi_lower_expr(l, p->exprs[i]);
        if (!arg_vals[i])
            return;
        /* Unit shares null's runtime tag, so a unit-typed argument would print
         * as `null`. Render it as `()` from its static type instead: the
         * expression above still runs for its side effects, only the shown
         * value changes. This mirrors how a function value renders as `<fn>`
         * rather than exposing its representation. */
        if (arg_vals[i]->type && arg_vals[i]->type->kind == XR_KIND_UNIT)
            arg_vals[i] = xi_const_str(l->func, l->cur_block, "()", l->type_string);
    }

    /* Emit one XI_PRINT per argument with correct spacing/newline flags.
     * aux_int encoding:
     *   bit0 = add_space   → OP_PRINT B field
     *   bit1 = newline     → OP_PRINT C bit0
     *   bits 2..3 = slot type hint → OP_PRINT C bits 1..2 (unused here)
     *   bit4 = skip_null   → OP_PRINT C bit3 (REPL auto-echo only) */
    int skip_null = p->skip_null ? 1 : 0;
    for (int i = 0; i < nargs; i++) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
        if (!v)
            return;
        v->args[0] = arg_vals[i];

        int add_space = (i > 0) ? 1 : 0;
        int newline = (i == nargs - 1) ? 1 : 0;
        int slot_hint = stmt_print_slot_hint_for_value(arg_vals[i]);
        v->aux_int = add_space | (newline << 1) | (slot_hint << 2) | (skip_null << 4);

        v->flags = xi_op_default_effects(XI_PRINT);
        v->line = (uint32_t) node->line;
    }
}

/* An enum variant constructed at the throw is the error value itself, so it is
 * allocated straight into the shared band. A unit variant needs no such
 * marking: it is an immutable per-enum singleton that is already shared. */
static bool stmt_throw_is_direct_enum_constructor(XiLower *l, AstNode *expr) {
    while (expr && (expr->type == AST_GROUPING || expr->type == AST_AS_EXPR))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.as_expr.expr;
    if (!l || !l->analyzer || !expr || expr->type != AST_CALL_EXPR || !expr->as.call_expr.callee ||
        expr->as.call_expr.callee->type != AST_MEMBER_ACCESS)
        return false;
    AstNode *object = expr->as.call_expr.callee->as.member_access.object;
    if (!object || object->type != AST_VARIABLE)
        return false;
    XaSymbol *symbol =
        object->as.variable.symbol_id
            ? xa_scope_lookup_by_id(l->analyzer->global_scope, object->as.variable.symbol_id)
            : NULL;
    return symbol && symbol->kind == XA_SYM_ENUM;
}

static void lower_throw(XiLower *l, AstNode *node) {
    ThrowStmtNode *t = &node->as.throw_stmt;
    XiBlock *allocation_block = l->cur_block;
    uint32_t allocation_begin = allocation_block ? allocation_block->nvalues : 0;
    XiValue *val = xi_lower_expr(l, t->expression);
    if (!val)
        return;
    if (allocation_block == l->cur_block) {
        stmt_mark_storage_allocs_in_range(allocation_block, allocation_begin,
                                          XR_OBJ_STORAGE_SHARED);
        if (stmt_throw_is_direct_enum_constructor(l, t->expression))
            xi_value_set_allocation_storage_mode(val, XR_OBJ_STORAGE_SHARED);
    }

    /* `throw <enum>` is the value-return error channel: write the error
     * and either branch to the enclosing error-catch (try_depth > 0) or
     * return it from the current function. */
    xi_lower_reprop_error(l, val, node);
}

static void lower_return(XiLower *l, AstNode *node) {
    ReturnStmtNode *ret = &node->as.return_stmt;
    XiValue *val = NULL;

    if (ret->value_count == 1 && ret->values[0]) {
        XiBlock *allocation_block = l->cur_block;
        uint32_t allocation_begin = allocation_block ? allocation_block->nvalues : 0;
        val = xi_lower_expr(l, ret->values[0]);
        if (l->func && l->func->return_storage_known && allocation_block == l->cur_block) {
            if (l->func->return_storage_domain == XR_STORAGE_TRANSFERABLE)
                stmt_mark_storage_allocs_in_range(allocation_block, allocation_begin,
                                                  XR_OBJ_STORAGE_TRANSFER);
            else if (l->func->return_storage_domain == XR_STORAGE_CONST_SHARED ||
                     l->func->return_storage_domain == XR_STORAGE_SYNC_SHARED)
                stmt_mark_storage_allocs_in_range(allocation_block, allocation_begin,
                                                  XR_OBJ_STORAGE_SHARED);
        }
        stmt_set_missing_line(val, node->line);
        val = xi_lower_apply_numeric_conversion_witness(l, ret->values[0], val,
                                                        l->func ? l->func->return_type : NULL);
        if (!val)
            return;
        /* A unit-typed return carries no control value: the expression above
         * is lowered only for its side effects, which are already emitted into
         * the current block. Dropping the value below leaves the RETURN block
         * with no control operand, which the IR verifier requires for a unit
         * return, and forcing is_direct_call false keeps tail-call marking off
         * a call that is no longer the block's control value. */
        bool return_is_unit =
            l->func && l->func->return_type && l->func->return_type->kind == XR_KIND_UNIT;
        /* Tail-call detection: mark calls in return position so the emitter
         * uses OP_TAILCALL / OP_INVOKE_TAIL (constant-space recursion).
         *
         * IMPORTANT: only apply when the AST return expression is directly
         * a call (AST_CALL_EXPR). If the return expression is a variable
         * that was assigned from a call, SSA propagation makes val->op
         * appear as XI_CALL_METHOD/XI_CALL, but intervening statements
         * (print, assignments) between the call and return make tail-call
         * optimization incorrect — it would discard those side effects.
         *
         * XI_CALL_METHOD → always safe (OP_INVOKE_TAIL handles all types).
         * XI_CALL with self_call flag → always safe (same closure).
         * XI_CALL with callee typed as function → safe.
         * Other XI_CALL (class constructors, etc.) → NOT safe; OP_TAILCALL
         * only handles closures and would fail on class objects. */
        bool is_direct_call = !return_is_unit && (ret->values[0]->type == AST_CALL_EXPR);
        /* A `T(args)` construction lowers to an XI_CALL_METHOD whose aux is
         * "constructor". Constructors must materialize and return the new
         * object, so they are never tail calls (and AOT has no TAIL_CALL). */
        bool is_constructor_call = val && val->op == XI_CALL_METHOD && val->aux &&
                                   strcmp((const char *) val->aux, "constructor") == 0;
        if (is_direct_call && !is_constructor_call && val && val->op == XI_CALL_METHOD) {
            val->flags |= XI_FLAG_TAIL;
        } else if (is_direct_call && val && val->op == XI_CALL) {
            bool is_self = (val->aux_int & 0xFF) == 1;
            bool callee_is_func = val->nargs >= 1 && val->args[0] && val->args[0]->type &&
                                  val->args[0]->type->kind == XR_KIND_FUNCTION;
            if (is_self || callee_is_func) {
                val->flags |= XI_FLAG_TAIL;
            }
        }
        if (!return_is_unit)
            val = xi_lower_checktype_for_type(l, node, val, l->func ? l->func->return_type : NULL);
        else
            val = NULL;
    } else if (ret->value_count > 1) {
        XR_CHECK(false, "obsolete multi-value return reached Xi lowering");
        return;
    }

    if (xi_lower_cleanup_has_active_site(l) && val) {
        val->flags &= (uint16_t) ~XI_FLAG_TAIL;
        XiValue *frozen = xi_value_new(l->func, l->cur_block, XI_COPY, val->type, 1);
        if (!frozen) {
            l->had_error = true;
            return;
        }
        frozen->args[0] = val;
        frozen->aux_int = XI_COPY_KIND_CLEANUP_RETURN;
        frozen->line = (uint32_t) node->line;
        val = frozen;
    }
    xi_lower_cleanup_run_to_depth(l, 0, node->line);

    int ret_line = node->line;
    if (ret_line <= 0 && ret->value_count == 1 && ret->values[0])
        ret_line = ret->values[0]->line;
    if (l->cur_block && ret_line > 0)
        l->cur_block->line = (uint32_t) ret_line;
    xi_block_set_return(l->cur_block, val);
    l->cur_block = NULL;
}

static void lower_block(XiLower *l, AstNode *node) {
    /* No scope push/pop needed: the analyzer assigns unique symbol_ids
     * to variables in different scopes, so shadowed variables naturally
     * get distinct var_id slots in the Braun SSA. */
    xi_lower_cleanup_scope_push(l);
    lower_stmts(l, node->as.block.statements, node->as.block.count);
    xi_lower_cleanup_scope_pop_normal(l, node->line);
}

static void lower_if(XiLower *l, AstNode *node) {
    IfStmtNode *s = &node->as.if_stmt;

    XiValue *cond = xi_lower_expr(l, s->condition);
    if (!cond || !l->cur_block)
        return;
    cond = xi_lower_bool_condition(l, cond);

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *else_blk = s->else_branch ? xi_block_new(l->func) : merge;

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);

    /* then_blk has 1 pred (cur_block) — seal immediately */
    xi_lower_braun_seal(l, then_blk);
    if (s->else_branch)
        xi_lower_braun_seal(l, else_blk);

    /* Then branch */
    l->cur_block = then_blk;
    xi_lower_stmt(l, s->then_branch);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* Else branch */
    if (s->else_branch) {
        l->cur_block = else_blk;
        xi_lower_stmt(l, s->else_branch);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* merge preds now fully known — seal and continue */
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

static void lower_while(XiLower *l, AstNode *node) {
    WhileStmtNode *s = &node->as.while_stmt;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* Jump to condition — cond_blk is a loop header (unsealed) */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: cond_blk NOT sealed yet (back edge pending) */
    l->cur_block = cond_blk;
    XiValue *cond = xi_lower_expr(l, s->condition);
    if (cond)
        cond = xi_lower_bool_condition(l, cond);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    /* body_blk has 1 pred (cond_blk) — seal immediately */
    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, cond_blk);

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block) /* back edge */
        xi_block_set_jump(l->cur_block, cond_blk);

    /* All preds of cond_blk now known (entry + back edge) — seal */
    xi_lower_braun_seal(l, cond_blk);

    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

static void lower_for(XiLower *l, AstNode *node) {
    ForStmtNode *s = &node->as.for_stmt;

    /* Initializer in current block */
    if (s->initializer)
        xi_lower_stmt(l, s->initializer);
    if (!l->cur_block)
        return;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* cond_blk is a loop header — do NOT seal yet */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition */
    l->cur_block = cond_blk;
    if (s->condition) {
        XiValue *cond = xi_lower_expr(l, s->condition);
        if (cond)
            cond = xi_lower_bool_condition(l, cond);
        if (cond)
            xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);
    } else {
        xi_block_set_jump(l->cur_block, body_blk);
    }

    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiLoopTarget loop_target;
    xi_lower_loop_push(l, &loop_target, s->label, exit_blk, incr_blk);

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    /* Increment */
    l->cur_block = incr_blk;
    if (s->increment) {
        if (incr_blk->npreds > 0)
            xi_lower_expr(l, s->increment);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    /* cond_blk back edge now added — seal */
    xi_lower_braun_seal(l, cond_blk);

    xi_lower_loop_pop(l, &loop_target);

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* lower_for_in_loop, lower_for_in_keyvalue, lower_for_in → xi_lower_stmt.c */

/* (function bodies removed — see xi_lower_stmt.c)
 * Remaining: lower_break, lower_continue kept here as they are tiny. */

static void lower_break(XiLower *l, AstNode *node) {
    XiLoopTarget *target = xi_lower_loop_find(l, node ? node->as.break_stmt.label : NULL);
    if (target && target->break_target && l->cur_block) {
        xi_lower_cleanup_run_to_depth(l, target->cleanup_scope_depth, node ? node->line : 0);
        xi_block_set_jump(l->cur_block, target->break_target);
        l->cur_block = NULL;
    }
}

static void lower_continue(XiLower *l, AstNode *node) {
    XiLoopTarget *target = xi_lower_loop_find(l, node ? node->as.continue_stmt.label : NULL);
    if (target && target->continue_target && l->cur_block) {
        xi_lower_cleanup_run_to_depth(l, target->cleanup_scope_depth, node ? node->line : 0);
        xi_block_set_jump(l->cur_block, target->continue_target);
        l->cur_block = NULL;
    }
}

/* Re-export: "export { a, b as c } from './file'" or "export * from './file'".
 * Records XiReexportEntry on XiFunc; emit_reexports() generates bytecodes. */
static bool ensure_reexport_capacity(XiFunc *f, uint16_t additional) {
    XR_DCHECK(f != NULL, "ensure_reexport_capacity: NULL func");
    if (!f || additional == 0)
        return true;
    uint32_t required = (uint32_t) f->reexport_count + additional;
    if (required <= f->reexport_capacity)
        return true;
    uint32_t capacity = f->reexport_capacity ? f->reexport_capacity : 4;
    while (capacity < required)
        capacity *= 2;
    if (capacity > UINT16_MAX)
        return false;
    XiReexportEntry *entries =
        (XiReexportEntry *) xi_func_arena_alloc(f, (uint32_t) (capacity * sizeof(XiReexportEntry)));
    if (!entries)
        return false;
    if (f->reexports && f->reexport_count > 0) {
        memcpy(entries, f->reexports, (size_t) f->reexport_count * sizeof(XiReexportEntry));
    }
    f->reexports = entries;
    f->reexport_capacity = (uint16_t) capacity;
    return true;
}

static void lower_reexport_stmt(XiLower *l, AstNode *node) {
    XR_DCHECK(l != NULL, "lower_reexport_stmt: NULL lowerer");
    XR_DCHECK(node != NULL, "lower_reexport_stmt: NULL node");
    ExportStmtNode *exp = &node->as.export_stmt;
    if (!exp->from_path)
        return;

    XiFunc *f = l->func;
    if (exp->is_reexport_all) {
        /* export * from "./file" — single entry with name=NULL */
        XiReexportEntry *e =
            (XiReexportEntry *) xi_func_arena_alloc(f, (uint32_t) sizeof(XiReexportEntry));
        if (!e)
            return;
        uint32_t pl = (uint32_t) strlen(exp->from_path);
        char *pc = (char *) xi_func_arena_alloc(f, pl + 1);
        if (pc)
            memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;
        e->name = NULL;
        e->alias = NULL;
        e->resolved_mod_index = -1;
        e->resolved_export_slot = -1;
        e->resolved_members = NULL;
        e->resolved_member_count = 0;
        e->resolution_attempted = false;
        e->resolution_complete = false;

        uint16_t idx = f->reexport_count;
        if (!ensure_reexport_capacity(f, 1))
            return;
        f->reexports[idx] = *e;
        f->reexport_count = idx + 1;
        return;
    }

    /* Selective re-export: export { a, b as c } from "./file" */
    for (int i = 0; i < exp->reexport_count; i++) {
        ReexportMember *m = &exp->reexport_members[i];
        if (!m->name)
            continue;

        uint16_t idx = f->reexport_count;
        if (!ensure_reexport_capacity(f, 1))
            return;

        XiReexportEntry *e = &f->reexports[idx];
        memset(e, 0, sizeof(*e));
        e->resolved_mod_index = -1;
        e->resolved_export_slot = -1;
        /* Arena-copy strings */
        uint32_t pl = (uint32_t) strlen(exp->from_path);
        char *pc = (char *) xi_func_arena_alloc(f, pl + 1);
        if (pc)
            memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;

        uint32_t nl = (uint32_t) strlen(m->name);
        char *nc = (char *) xi_func_arena_alloc(f, nl + 1);
        if (nc)
            memcpy(nc, m->name, nl + 1);
        e->name = nc;

        if (m->alias) {
            uint32_t al = (uint32_t) strlen(m->alias);
            char *ac = (char *) xi_func_arena_alloc(f, al + 1);
            if (ac)
                memcpy(ac, m->alias, al + 1);
            e->alias = ac;
        } else {
            e->alias = NULL;
        }
        f->reexport_count = idx + 1;
    }
}

XR_FUNC bool xi_lower_import_member_is_type_only(const XiLower *l, const ImportMember *member) {
    XaSymbol *symbol;
    if (!l || !l->analyzer || !l->analyzer->global_scope || !member || member->symbol_id == 0)
        return false;
    symbol = xa_scope_lookup_by_id(l->analyzer->global_scope, member->symbol_id);
    return xa_symbol_is_type_alias(symbol);
}

/* Selective import: import { square, cube } from "./math_lib"
 * Creates XI_IMPORT_REF values for runtime members and binds them as local
 * variables. Type-only members are already available to semantic type
 * resolution and intentionally have no runtime slot. The AOT driver resolves
 * module_path + member_name to the target module's shared slot after all
 * modules are lowered. */
static void lower_import_stmt(XiLower *l, AstNode *node) {
    XR_DCHECK(l != NULL, "lower_import_stmt: NULL lowerer");
    XR_DCHECK(node != NULL, "lower_import_stmt: NULL node");
    ImportStmtNode *imp = &node->as.import_stmt;

    /* Whole-module import: import math / import math as m.
     * Emit XI_IMPORT_REF with member_name=NULL so xi_emit generates
     * OP_LOAD_MODULE, binding the module object itself. */
    if (imp->member_count == 0) {
        const char *local_name = imp->alias ? imp->alias : imp->module_name;
        if (!local_name)
            return;
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref =
            (XiImportRef *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        memset(ref, 0, sizeof(*ref));
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;
        ref->resolved_export_slot = -1;
        if (imp->module_name) {
            uint32_t ml = (uint32_t) strlen(imp->module_name);
            char *mc = (char *) xi_func_arena_alloc(l->func, ml + 1);
            if (mc) {
                memcpy(mc, imp->module_name, ml + 1);
                ref->module_path = mc;
            }
        }

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v)
            return;
        v->aux = (void *) ref;
        v->aux_int = -1;
        v->line = (uint32_t) node->line;

        int var_id = xi_lower_var_create(l, imp->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into backing store so nested functions can access */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            if (b.slot < l->var_cap)
                l->shared_slot_imports[b.slot] = ref;
            xi_lower_emit_top_store(l, b, v);
        }
        return;
    }

    for (int i = 0; i < imp->member_count; i++) {
        ImportMember *m = &imp->members[i];
        if (xi_lower_import_member_is_type_only(l, m))
            continue;
        const char *local_name = m->alias ? m->alias : m->name;

        if (xi_lower_import_member_is_type_only(l, m))
            continue;

        if (imp->module_name && strcmp(imp->module_name, "sync") == 0 &&
            xi_lower_sync_runtime_class_global_index(m->name) >= 0) {
            struct XrType *type = xr_type_new_class(NULL, m->name);
            XiValue *v = xi_lower_emit_sync_runtime_builtin_class(l, m->name, (int) node->line);
            if (!v)
                return;

            int var_id = xi_lower_var_create(l, m->symbol_id, local_name, type);
            if (var_id >= 0 && var_id < l->var_count)
                l->vars[var_id].type = type;
            xi_lower_braun_write(l, var_id, l->cur_block, v);

            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiTopBinding b;
                b.slot = l->shared_map[var_id];
                b.name = l->vars[var_id].name;
                b.type = type;
                xi_lower_emit_top_store(l, b, v);
            }
            continue;
        }

        /* Create XI_IMPORT_REF carrying module path and member name */
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref =
            (XiImportRef *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        memset(ref, 0, sizeof(*ref));
        /* Copy strings into arena so they survive AST destruction */
        if (imp->module_name) {
            uint32_t ml = (uint32_t) strlen(imp->module_name);
            char *mc = (char *) xi_func_arena_alloc(l->func, ml + 1);
            if (mc) {
                memcpy(mc, imp->module_name, ml + 1);
                ref->module_path = mc;
            }
        }
        if (m->name) {
            uint32_t nl = (uint32_t) strlen(m->name);
            char *nc = (char *) xi_func_arena_alloc(l->func, nl + 1);
            if (nc) {
                memcpy(nc, m->name, nl + 1);
                ref->member_name = nc;
            }
        }
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;
        ref->resolved_export_slot = -1;

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v)
            return;
        v->aux = (void *) ref;
        v->aux_int = -1;
        v->line = (uint32_t) node->line;

        /* Bind as a local variable so subsequent references resolve */
        int var_id = xi_lower_var_create(l, m->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into backing store so nested functions can access.
         * Without this mirror, nested scopes see null for the imported
         * member because the read path emits the matching load via the
         * top-binding helper. */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            if (b.slot < l->var_cap)
                l->shared_slot_imports[b.slot] = ref;
            xi_lower_emit_top_store(l, b, v);
        }
    }
}

/* Main statement dispatcher */
/* Module-system and type-declaration statements, split out of the main
 * dispatch switch. Returns false when `node` is not one of them. */
static bool lower_module_or_type_decl_stmt(XiLower *l, AstNode *node) {
    switch (node->type) {
        /* Module system: imports bind locally; export statements are re-exports. */
        case AST_IMPORT_STMT:
            lower_import_stmt(l, node);
            return true;
        case AST_EXPORT_STMT:
            if (node->as.export_stmt.from_path)
                lower_reexport_stmt(l, node);
            return true;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            xi_lower_class_decl(l, node);
            return true;
        case AST_INTERFACE_DECL:
        case AST_TYPE_ALIAS:
            return true;
        case AST_ENUM_DECL:
            xi_lower_enum_decl(l, node);
            return true;
        case AST_GLOBAL_ASM:
            xi_lower_record_global_asm(l, node);
            return true;
        default:
            return false;
    }
}

XR_FUNC void xi_lower_stmt(XiLower *l, AstNode *node) {
    if (!node)
        return;
    if (!l->cur_block)
        return; /* dead code */

    if (lower_module_or_type_decl_stmt(l, node))
        return;

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            lower_var_decl(l, node);
            break;

        case AST_EXPR_STMT: {
            if (lower_is_comptime_block_expr(node->as.expr_stmt))
                break;
            XiValue *expr = xi_lower_expr(l, node->as.expr_stmt);
            if (expr && expr->op == XI_GO)
                expr->flags |= XI_FLAG_FIRE_AND_FORGET;
        } break;

        case AST_PRINT_STMT:
            lower_print(l, node);
            break;

        case AST_RETURN_STMT:
            lower_return(l, node);
            break;

        case AST_BLOCK:
            lower_block(l, node);
            break;

        case AST_IF_STMT:
            lower_if(l, node);
            break;

        case AST_WHILE_STMT:
            lower_while(l, node);
            break;

        case AST_FOR_STMT:
            lower_for(l, node);
            break;

        case AST_FOR_IN_STMT:
            xi_lower_for_in(l, node);
            break;

        case AST_BREAK_STMT:
            lower_break(l, node);
            break;

        case AST_CONTINUE_STMT:
            lower_continue(l, node);
            break;

        case AST_THROW_STMT:
            lower_throw(l, node);
            break;

        case AST_TRY_CATCH:
            xi_lower_try_catch(l, node);
            break;

        /* Function declaration as statement */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            xi_lower_function_decl(l, node);
            break;

        case AST_DEFER_STMT:
            lower_defer(l, node);
            break;

        /* Select statement (channel multiplexing) */
        case AST_SELECT_STMT:
            xi_lower_select(l, node);
            break;

        /* Scope block (structured concurrency) */
        case AST_SCOPE_BLOCK:
            xi_lower_scope_block(l, node);
            break;

        /* Yield execution */
        case AST_YIELD_STMT:
            lower_yield_stmt(l, node);
            break;

        /* Destructuring */
        case AST_DESTRUCTURE_DECL:
            lower_destructure_decl(l, node);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            lower_destructure_assign(l, node);
            break;

        /* Match expression used as statement */
        case AST_MATCH_EXPR:
            xi_lower_expr(l, node);
            break;

        /* Expressions that appear as statements (assignment, call, etc.) */
        case AST_ASSIGNMENT:
        case AST_CALL_EXPR:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
        case AST_COMPTIME_EXPR:
            if (lower_is_comptime_block_expr(node))
                break;
            xi_lower_expr(l, node);
            break;
        case AST_GO_EXPR:
        case AST_AWAIT_EXPR:
        case AST_NEW_EXPR:
        case AST_MOVE_EXPR:
        case AST_UNSAFE_EXPR:
            xi_lower_expr(l, node);
            break;

        default:
            /* Every analyzer-accepted AST node must be lowerable.
             * Reaching here indicates a compiler bug, not a user error. */
            XR_DCHECK_FMT(false, "unsupported stmt AST kind %d in lowering", (int) node->type);
            l->had_error = true;
            break;
    }
}

static void prescan_block_decls(XiLower *l, AstNode **stmts, int count) {
    /* Pre-register declarations as Braun SSA variables so hoisted function
     * bodies can resolve forward references.
     *
     * Functions: get a null placeholder value (needed for register allocation
     * and cell-based upvalue capture) marked with SIDE_EFFECT to survive DCE.
     *
     * Bindings (var/const/shared): only create the variable slot without writing a
     * null placeholder — the actual initializer assigns the register.
     * This avoids cell-wrapping conflicts where the null occupies the register
     * before the real initialization overwrites it. */
    for (int i = 0; i < count; i++) {
        AstNode *s = stmts[i];
        if (!s)
            continue;
        const char *name = NULL;
        uint32_t sid = 0;
        struct XrType *type = NULL;
        bool is_func = false;
        switch (s->type) {
            case AST_FUNCTION_DECL:
                name = s->as.function_decl.name;
                sid = s->as.function_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                is_func = true;
                break;
            case AST_VAR_DECL:
            case AST_CONST_DECL:
                name = s->as.var_decl.name;
                sid = s->as.var_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                break;
            default:
                continue;
        }
        if (!name)
            continue;
        int var_id = xi_lower_var_find(l, sid, name);
        if (var_id < 0) {
            var_id = xi_lower_var_create(l, sid, name, type);
            if (is_func) {
                XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
                if (null_val)
                    null_val->flags |= XI_FLAG_SIDE_EFFECT;
                xi_lower_braun_write(l, var_id, l->cur_block, null_val);
            }
        }
        if (is_func)
            l->vars[var_id].hoisted = true;
    }
}

static void lower_stmts(XiLower *l, AstNode **stmts, int count) {
    /* Pre-register declarations and hoist function bodies.
     * Function bodies are lowered first so same-scope forward calls
     * (e.g. calling greetBlock before its declaration) resolve to an
     * actual closure rather than the null placeholder. */
    /* At module level, shared variables already handle forward references
     * for program-level functions.  Hoisting only applies inside function
     * bodies where nested functions capture sibling function variables. */
    bool in_loop = (l->break_target != NULL);
    if (l->cur_block && !l->is_program && !in_loop) {
        prescan_block_decls(l, stmts, count);
        for (int i = 0; i < count; i++) {
            if (!l->cur_block)
                break;
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL && s->as.function_decl.name != NULL)
                xi_lower_stmt(l, s);
        }
        /* After hoisting, mark parent variables that are captured by any
         * hoisted child. Hoisting reorders closures before variable
         * initializers, so the initializer has no IR uses (the capture
         * already bound to the braun-read null placeholder). Marking
         * keeps the initializer alive through DCE. */
        for (uint16_t ci = 0; ci < l->func->nchildren; ci++) {
            XiFunc *child = l->func->children[ci];
            if (!child)
                continue;
            for (uint16_t cj = 0; cj < child->ncaptures; cj++) {
                XiCapture *cap = &child->captures[cj];
                if (cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                /* Resolve capture name back to parent var_id */
                int vid = -1;
                if (cap->value && xi_var_id_is_valid(cap->value->var_id))
                    vid = (int) cap->value->var_id;
                else if (cap->name)
                    vid = xi_lower_var_find(l, 0, cap->name);
                if (vid >= 0 && vid < l->var_count)
                    l->vars[vid].captured_by_child = true;
            }
        }
        for (int i = 0; i < count; i++) {
            if (!l->cur_block)
                break; /* dead code after return/break */
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL && s->as.function_decl.name != NULL)
                continue; /* already hoisted */
            xi_lower_stmt(l, s);
        }
    } else {
        for (int i = 0; i < count; i++) {
            if (!l->cur_block)
                break;
            xi_lower_stmt(l, stmts[i]);
        }
    }
}
