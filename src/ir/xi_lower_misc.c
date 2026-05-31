/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_misc.c - Miscellaneous expression lowering helpers
 *
 * Contains: enum access/decl/convert, object literal, catch expr,
 * cancelled/move lowering.  Extracted from xi_lower_expr.c to keep
 * individual translation units within the 3000-line limit.
 */

#include "xi_lower_internal.h"
#include "xi.h"
#include "xi_effect.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xclass.h"
#include "../runtime/object/xstring.h"
#include "../base/xglobal_indices.h"

#include <string.h>
#include <stdio.h>

/* ========== Enum Access ========== */

/* Prelude enums (Result, Ordering) have a single canonical XrEnumType bound
 * into a VM builtin slot; they are not per-module declarations.  Returns the
 * builtin global index, or -1 for ordinary user enums. */
static int prelude_enum_builtin_index(const char *enum_name) {
    if (!enum_name)
        return -1;
    if (strcmp(enum_name, "Result") == 0)
        return XR_GLOBAL_VAR_RESULT;
    if (strcmp(enum_name, "Ordering") == 0)
        return XR_GLOBAL_VAR_ORDERING;
    return -1;
}

XR_FUNC XiValue *xi_lower_enum_access(XiLower *l, AstNode *node) {
    EnumAccessNode *ea = &node->as.enum_access;
    XR_DCHECK(ea->enum_name != NULL, "enum access must have enum name");

    /* Resolve the enum type value, then GETPROP for the member.  Prelude
     * enums resolve to a shared builtin slot; user enums to the shared
     * variable created by their declaration. */
    XiValue *enum_val;
    int builtin_idx = prelude_enum_builtin_index(ea->enum_name);
    if (builtin_idx >= 0) {
        enum_val = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, l->type_any, 0);
        if (!enum_val)
            return NULL;
        enum_val->aux_int = builtin_idx;
        enum_val->aux = (void *) arena_strdup(l->func, ea->enum_name);
        enum_val->line = (uint32_t) node->line;
    } else {
        int var_id = xi_lower_var_create(l, 0, ea->enum_name, l->type_any);
        enum_val = xi_lower_braun_read(l, var_id, l->cur_block);
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = enum_val;
    v->aux = (void *) arena_strdup(l->func, ea->member_name);
    v->line = (uint32_t) node->line;
    return v;
}

/* ========== Enum Constant Evaluation ========== */

/* Evaluate compile-time constant for enum member values. */
static XrValue enum_eval_const(XiLower *l, AstNode *expr) {
    if (!expr)
        return xr_null();
    switch (expr->type) {
        case AST_LITERAL_INT:
            return xr_int(expr->as.literal.raw_value.int_val);
        case AST_LITERAL_FLOAT:
            return xr_float(expr->as.literal.raw_value.float_val);
        case AST_LITERAL_STRING: {
            const char *s = expr->as.literal.raw_value.string_val;
            XrString *xs = xr_compile_time_intern(l->isolate, s, strlen(s));
            return xr_string_value(xs);
        }
        case AST_LITERAL_TRUE:
            return xr_bool(true);
        case AST_LITERAL_FALSE:
            return xr_bool(false);
        default:
            return xr_null();
    }
}

/* ========== Enum Declaration ========== */

/* Lower AST_ENUM_DECL: create XrEnumType at compile time, store as
 * shared variable so enum member access can find it.
 * Handles both simple enums (backing values) and ADT enums (tag + payload). */
XR_FUNC void xi_lower_enum_decl(XiLower *l, AstNode *node) {
    EnumDeclNode *ed = &node->as.enum_decl;
    XR_DCHECK(ed->name != NULL, "enum name must not be NULL");
    XR_DCHECK(l->isolate != NULL, "isolate required for enum creation");

    int n = ed->member_count;
    char **names = (char **) xr_malloc(sizeof(char *) * (size_t) n);
    XrValue *values = (XrValue *) xr_malloc(sizeof(XrValue) * (size_t) n);
    if (!names || !values) {
        xr_free(names);
        xr_free(values);
        return;
    }

    /* Detect ADT enum: any variant with payload_count > 0 */
    bool is_adt = false;
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        if (m->payload_count > 0) {
            is_adt = true;
            break;
        }
    }

    int detected_base = XR_TINT;
    int64_t auto_val = 0;
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        names[i] = strdup(m->name);
        if (is_adt) {
            /* ADT enum: all variants get tag index as backing value */
            values[i] = xr_int(i);
        } else if (m->value) {
            values[i] = enum_eval_const(l, m->value);
            if (XR_IS_INT(values[i])) {
                auto_val = XR_TO_INT(values[i]) + 1;
            } else if (XR_IS_STRING(values[i])) {
                detected_base = XR_TSTRING;
            } else if (XR_IS_FLOAT(values[i])) {
                detected_base = XR_TFLOAT;
            } else if (XR_IS_BOOL(values[i])) {
                detected_base = XR_TBOOL;
            }
        } else {
            values[i] = xr_int(auto_val);
            auto_val++;
        }
    }

    XrEnumType *et = xr_enum_type_new(l->isolate, ed->name, detected_base, names, values, n);
    /* names/values ownership transferred to xr_enum_type_new */

    /* Set ADT metadata on the created enum type */
    if (et && is_adt) {
        et->is_adt = true;
        et->payload_counts = (int *) xr_calloc((size_t) n, sizeof(int));
        int max_pc = 0;
        if (et->payload_counts) {
            for (int i = 0; i < n; i++) {
                int pc = ed->members[i]->as.enum_member.payload_count;
                et->payload_counts[i] = pc;
                if (pc > max_pc)
                    max_pc = pc;
            }
        }
        et->max_payload = max_pc;

        /* Set field_count on the enum class so xr_instance_new allocates
         * space for variant tag (field[0]) + payload (field[1..max_payload]).
         * Set builtin_kind = XR_BK_ADT_ENUM so the formatter can detect it. */
        if (et->enum_class && max_pc > 0) {
            et->enum_class->field_count = (uint16_t) (1 + max_pc);
            et->enum_class->own_field_count = (uint16_t) (1 + max_pc);
            // ADT enum identity is now tracked via builtin_kind
            et->enum_class->builtin_kind = XR_BK_ADT_ENUM;
        }
    }

    /* Store as XI_CONST with type_any (emitter handles via LOADK) */
    XiValue *cv = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
    if (!cv)
        return;
    cv->aux = (void *) et;
    cv->line = (uint32_t) node->line;

    /* Write to shared variable so enum access resolves correctly */
    int var_id = xi_lower_var_create(l, ed->symbol_id, ed->name, l->type_any);
    xi_lower_braun_write(l, var_id, l->cur_block, cv);

    if (l->is_program && var_id < l->var_count && l->shared_map[var_id] >= 0) {
        XiTopBinding binding;
        binding.slot = l->shared_map[var_id];
        binding.name = l->vars[var_id].name;
        binding.type = l->type_any;
        xi_lower_emit_top_store(l, binding, cv);
    }
}

/* ========== Enum Convert ========== */

XR_FUNC XiValue *xi_lower_enum_convert(XiLower *l, AstNode *node) {
    EnumConvertNode *ec = &node->as.enum_convert;
    XiValue *val = xi_lower_expr(l, ec->value_expr);
    if (!val)
        return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = val;
    /* tid=-1 (unknown) → emitter degenerates to move, which is correct
     * for enum conversions (runtime handles via enum type metadata). */
    v->aux_int = (-1 << 1) | 0;
    v->aux = (void *) arena_strdup(l->func, ec->enum_name);
    v->line = (uint32_t) node->line;
    return v;
}

/* ========== Cancelled / Move ========== */

XR_FUNC XiValue *xi_lower_cancelled_expr(XiLower *l, AstNode *node) {
    /* cancelled() returns bool */
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_bool, 0);
    if (!v)
        return NULL;
    v->aux_int = 0; /* builtin id for 'cancelled' */
    v->line = (uint32_t) node->line;
    return v;
}

XR_FUNC XiValue *xi_lower_move_expr(XiLower *l, AstNode *node) {
    /* move var — transfer ownership; semantically same as reading the var */
    MoveExprNode *me = &node->as.move_expr;
    XiValue *val = xi_lower_expr(l, me->expr);
    if (!val)
        return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (!v)
        return val;
    v->args[0] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

/* ========== Catch Expression ========== */

/* Helper: emit XI_CALL_METHOD(receiver, method_name, arg) → OP_INVOKE.
 * Used by catch! lowering to call Result.Ok(v) / Result.Err(e). */
static XiValue *emit_method_call_1(XiLower *l, XiValue *recv, const char *method, XiValue *arg,
                                   uint32_t line) {
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 2);
    if (!v)
        return NULL;
    v->args[0] = recv;
    v->args[1] = arg;
    v->aux = (void *) arena_strdup(l->func, method);
    v->aux_int = (int64_t) xi_lower_method_symbol(l, method) << 1;
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = line;
    return v;
}

/* Lower catch! { body } expression.
 * Desugars into:
 *   try { body → Result.Ok(last_value) }
 *   catch (e) { Result.Err(e) }
 * Returns the Result value via Braun SSA merge of both paths. */
XR_FUNC XiValue *xi_lower_catch_expr(XiLower *l, AstNode *node) {
    CatchExprNode *ce = &node->as.catch_expr;
    AstNode *body = ce->body;
    XR_DCHECK(body != NULL, "catch! body must not be NULL");
    uint32_t line = (uint32_t) node->line;

    /* Synthetic Braun variable to carry the Result across try/catch paths */
    int catch_res_var = xi_lower_var_create(l, 0, "__catch_result", l->type_any);

    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    /* Emit XI_TRY — registers catch_blk as the exception handler */
    XiValue *try_op = xi_value_new(l->func, l->cur_block, XI_TRY, l->type_unit, 0);
    if (try_op) {
        try_op->aux = (void *) catch_blk;
        try_op->aux_int = -1; /* no finally */
        try_op->flags |= XI_FLAG_SIDE_EFFECT;
        try_op->line = line;
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    /* === Try body === */
    l->cur_block = try_blk;
    l->dead_after_throw = false;
    l->try_depth++;

    /* Lower all body statements; last expression-stmt provides the value */
    XiValue *body_val = NULL;
    if (body->type == AST_BLOCK) {
        BlockNode *blk = &body->as.block;
        for (int i = 0; i < blk->count; i++) {
            AstNode *stmt = blk->statements[i];
            if (!stmt || !l->cur_block)
                continue;
            if (i == blk->count - 1 && stmt->type == AST_EXPR_STMT) {
                body_val = xi_lower_expr(l, stmt->as.expr_stmt);
            } else {
                xi_lower_stmt(l, stmt);
            }
        }
    } else {
        body_val = xi_lower_expr(l, body);
    }
    if (!body_val && l->cur_block)
        body_val = xi_const_null(l->func, l->cur_block, l->type_null);

    l->try_depth--;

    /* Resolve the prelude Result enum variable.
     * xi_lower_var_find does name-based fallback regardless of symbol_id,
     * so it finds the variable created by xi_lower_enum_decl. */
    int result_var_id = xi_lower_var_find(l, 0, "Result");
    XR_DCHECK(result_var_id >= 0, "prelude Result enum must be available");

    /* Wrap success value: Result.Ok(body_val) */
    if (l->cur_block && body_val) {
        XiValue *rt = xi_lower_braun_read(l, result_var_id, l->cur_block);
        XiValue *ok = emit_method_call_1(l, rt, "Ok", body_val, line);
        if (ok)
            xi_lower_braun_write(l, catch_res_var, l->cur_block, ok);
    }

    XiBlock *try_exit_blk = l->cur_block;
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* === Catch body === */
    XiBlock *catch_pred = try_exit_blk ? try_exit_blk : try_blk;
    xi_block_add_pred(catch_blk, catch_pred);
    xi_lower_braun_seal(l, catch_blk);
    l->cur_block = catch_blk;
    l->dead_after_throw = false;

    XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_CATCH, l->type_any, 0);
    if (catch_op) {
        catch_op->flags |= XI_FLAG_SIDE_EFFECT;
        catch_op->line = line;
    }

    /* Wrap exception: Result.Err(exception) */
    if (l->cur_block && catch_op) {
        XiValue *rt = xi_lower_braun_read(l, result_var_id, l->cur_block);
        XiValue *err = emit_method_call_1(l, rt, "Err", catch_op, line);
        if (err)
            xi_lower_braun_write(l, catch_res_var, l->cur_block, err);
    }

    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* === Merge block === */
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    l->dead_after_throw = false;

    if (!l->cur_block)
        return xi_const_null(l->func, try_blk, l->type_null);

    XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
    if (end_op) {
        end_op->flags |= XI_FLAG_SIDE_EFFECT;
        end_op->line = line;
    }

    /* Braun SSA read at merge point automatically inserts PHI */
    return xi_lower_braun_read(l, catch_res_var, l->cur_block);
}

/* ========== Object Literal ========== */

XR_FUNC XiValue *xi_lower_object_literal(XiLower *l, AstNode *node) {
    ObjectLiteralNode *obj = &node->as.object_literal;
    int count = obj->count;
    int n = count > 32 ? 32 : count;

    /* Evaluate all values and computed key expressions first */
    XiValue *val_vals[32];
    XiValue *key_vals[32];
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, obj->values[i]);
        bool is_computed = obj->computed && obj->computed[i];
        key_vals[i] = is_computed ? xi_lower_expr(l, obj->keys[i]) : NULL;
    }

    /* Count static (non-computed) keys for Shape construction */
    int static_count = 0;
    for (int i = 0; i < n; i++) {
        if (!key_vals[i])
            static_count++;
    }

    /* Collect static key names (arena-allocated) */
    const char **key_names = (const char **) xi_func_arena_alloc(
        l->func, (uint32_t) (sizeof(const char *) * (static_count > 0 ? static_count : 1)));
    if (!key_names)
        return NULL;
    int si = 0;
    int static_idx_map[32]; /* maps static slot → Shape field index */
    for (int i = 0; i < n; i++) {
        if (!key_vals[i]) {
            if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING)
                key_names[si] = obj->keys[i]->as.literal.raw_value.string_val;
            else
                key_names[si] = "?";
            static_idx_map[i] = si;
            si++;
        } else {
            static_idx_map[i] = -1;
        }
    }

    /* Create Json object with Shape built from static keys only */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_JSON_NEW, result_type, 0);
    if (!obj_val)
        return NULL;
    obj_val->aux_int = static_count;
    obj_val->aux = (void *) key_names;
    obj_val->line = (uint32_t) node->line;

    /* Init static fields by index, computed fields by dynamic key */
    for (int i = 0; i < n; i++) {
        if (!key_vals[i]) {
            /* Static key → indexed init */
            XiValue *init = xi_value_new(l->func, l->cur_block, XI_JSON_INIT_F, l->type_unit, 2);
            if (!init)
                break;
            init->args[0] = obj_val;
            init->args[1] = val_vals[i];
            init->aux_int = static_idx_map[i];
            init->flags |= XI_FLAG_SIDE_EFFECT;
        } else {
            /* Computed key → dynamic index-set: obj[key] = val */
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
            if (!set)
                break;
            set->args[0] = obj_val;
            set->args[1] = key_vals[i];
            set->args[2] = val_vals[i];
            set->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
    return obj_val;
}
