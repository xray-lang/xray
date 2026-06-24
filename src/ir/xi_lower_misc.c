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

/* Prelude enums have a single canonical XrEnumType bound
 * into a VM builtin slot; they are not per-module declarations.  Returns the
 * builtin global index, or -1 for ordinary user enums. */
static int prelude_enum_builtin_index(const char *enum_name) {
    if (!enum_name)
        return -1;
    if (strcmp(enum_name, "Ordering") == 0)
        return XR_GLOBAL_VAR_ORDERING;
    if (strcmp(enum_name, "Recv") == 0)
        return XR_GLOBAL_VAR_RECV;
    if (strcmp(enum_name, "SendResult") == 0)
        return XR_GLOBAL_VAR_SEND_RESULT;
    if (strcmp(enum_name, "TaskResult") == 0)
        return XR_GLOBAL_VAR_TASK_RESULT;
    if (strcmp(enum_name, "TaskStatus") == 0)
        return XR_GLOBAL_VAR_TASK_STATUS;
    return -1;
}

static XiValue *lower_enum_method_closure(XiLower *l, XiFunc *child, uint16_t child_idx,
                                          struct XrType *fn_type, int line) {
    if (!l || !child)
        return NULL;
    uint16_t ncap = child->ncaptures;
    XiValue *closure =
        xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, fn_type ? fn_type : l->type_any, ncap);
    if (!closure)
        return NULL;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        closure->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    closure->aux = (void *) child;
    closure->aux_int = child_idx;
    closure->line = (uint32_t) line;
    return closure;
}

static void lower_enum_methods(XiLower *l, EnumDeclNode *ed) {
    if (!l || !ed || !ed->name || ed->method_count <= 0 || !l->analyzer)
        return;
    XaSymbol *enum_sym = xa_analyzer_lookup(l->analyzer, ed->name);
    XaSymbolLinks *enum_links = enum_sym ? xa_analyzer_get_links(l->analyzer, enum_sym) : NULL;
    XrClassInfo *info = enum_links ? enum_links->class_info : NULL;
    if (!info)
        return;

    for (int i = 0; i < ed->method_count; i++) {
        AstNode *method = ed->methods ? ed->methods[i] : NULL;
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
        if (!method_sym || method_sym->kind != XA_SYM_METHOD ||
            method_sym->is_static != md->is_static)
            continue;
        XaSymbolLinks *method_links = xa_analyzer_get_links(l->analyzer, method_sym);
        struct XrType *receiver_type =
            md->is_static ? NULL : xr_type_new_enum(l->isolate, ed->name);
        XiFunc *mf = xi_lower_method_as_func(l, md, !md->is_static, NULL, receiver_type);
        if (!mf) {
            l->had_error = true;
            continue;
        }
        xi_lower_func_add_child(l->func, mf);
        uint16_t child_idx = (uint16_t) (l->func->nchildren - 1);
        XiValue *closure = lower_enum_method_closure(
            l, mf, child_idx, method_links ? method_links->type : l->type_any, method->line);
        if (!closure) {
            l->had_error = true;
            continue;
        }

        const char *hidden =
            xi_lower_enum_method_hidden_name(l->func, ed->name, md->name, md->is_static);
        int var_id = xi_lower_var_find(l, method_sym->id, hidden);
        if (var_id < 0)
            var_id = xi_lower_var_create(l, method_sym->id, hidden, closure->type);
        xi_lower_braun_write(l, var_id, l->cur_block, closure);

        if (l->is_program && var_id < l->var_count && l->shared_map[var_id] >= 0) {
            int slot = l->shared_map[var_id];
            XiTopBinding b;
            b.slot = slot;
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, closure);
            if (slot >= 0 && slot < l->var_cap) {
                l->shared_slot_funcs[slot] = mf;
                if (l->func->shared_slot_funcs && slot < (int) l->func->shared_slot_func_count)
                    l->func->shared_slot_funcs[slot] = mf;
            }
        }
    }
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
    v->aux_int = xi_lower_method_symbol(l, ea->member_name);
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

static void enum_member_data_set_int(XiEnumMemberData *dst, int64_t value) {
    dst->value_kind = XI_ENUM_LITERAL_INT;
    dst->int_value = value;
}

static void enum_member_data_set_literal(XiLower *l, XiEnumMemberData *dst, AstNode *expr,
                                         int64_t fallback_int) {
    if (!dst)
        return;
    if (!expr) {
        enum_member_data_set_int(dst, fallback_int);
        return;
    }
    switch (expr->type) {
        case AST_LITERAL_INT:
            enum_member_data_set_int(dst, expr->as.literal.raw_value.int_val);
            break;
        case AST_LITERAL_FLOAT:
            dst->value_kind = XI_ENUM_LITERAL_FLOAT;
            dst->float_value = expr->as.literal.raw_value.float_val;
            break;
        case AST_LITERAL_STRING:
            dst->value_kind = XI_ENUM_LITERAL_STRING;
            dst->string_value = arena_strdup(l->func, expr->as.literal.raw_value.string_val);
            break;
        case AST_LITERAL_TRUE:
            dst->value_kind = XI_ENUM_LITERAL_BOOL;
            dst->bool_value = true;
            break;
        case AST_LITERAL_FALSE:
            dst->value_kind = XI_ENUM_LITERAL_BOOL;
            dst->bool_value = false;
            break;
        default:
            dst->value_kind = XI_ENUM_LITERAL_NULL;
            break;
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
    XiEnumData *enum_data = (XiEnumData *) xi_func_arena_alloc(l->func, sizeof(XiEnumData));
    XiEnumMemberData *enum_members = NULL;
    if (enum_data) {
        memset(enum_data, 0, sizeof(*enum_data));
        enum_members = (XiEnumMemberData *) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(XiEnumMemberData) * (size_t) n));
        if (enum_members) {
            memset(enum_members, 0, sizeof(XiEnumMemberData) * (size_t) n);
            enum_data->name = arena_strdup(l->func, ed->name);
            enum_data->member_count = (uint32_t) n;
            enum_data->is_adt = is_adt;
            enum_data->members = enum_members;
        } else {
            enum_data = NULL;
        }
    }
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        names[i] = strdup(m->name);
        if (enum_members) {
            enum_members[i].name = arena_strdup(l->func, m->name);
            enum_members[i].payload_count = m->payload_count;
        }
        if (is_adt) {
            /* ADT enum: all variants get tag index as backing value */
            values[i] = xr_int(i);
            if (enum_members)
                enum_member_data_set_int(&enum_members[i], i);
        } else if (m->value) {
            values[i] = enum_eval_const(l, m->value);
            if (enum_members)
                enum_member_data_set_literal(l, &enum_members[i], m->value, auto_val);
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
            if (enum_members)
                enum_member_data_set_int(&enum_members[i], auto_val);
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
        if (enum_data)
            enum_data->max_payload = max_pc;

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
    if (enum_data) {
        enum_data->base_type = detected_base;
        enum_data->runtime_type = et;
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
        if (binding.slot < l->var_cap)
            l->shared_slot_enums[binding.slot] = enum_data;
    }

    lower_enum_methods(l, ed);
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
    v->aux_int = ((int64_t) (uint32_t) -1 << 1) | 0;
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
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_MOVE, result_type, 1);
    if (!v)
        return val;
    v->args[0] = val;
    v->line = (uint32_t) node->line;
    return v;
}

/* ========== Object Literal ========== */

/* Object literal with `...spread` entries: `{...base, x: 1}`.
 * The result Json is created pre-sized with the statically-known union shape
 * (from the analyzer's inferred type), then each part is applied in order:
 * spread sources are merged field-by-field (XI_JSON_MERGE), literal fields are
 * written by key (XI_INDEX_SET). Later writes override earlier ones, matching
 * the union semantics. Dynamic source fields not in the static shape are added
 * at runtime via overflow. */
static XiValue *xi_lower_object_literal_spread(XiLower *l, AstNode *node,
                                               struct XrType *result_type) {
    ObjectLiteralNode *obj = &node->as.object_literal;

    /* Pre-size the result with the union of statically-known field names. */
    int static_count = 0;
    const char **key_names = NULL;
    if (result_type && result_type->kind == XR_KIND_JSON && result_type->object.field_count > 0 &&
        result_type->object.field_names) {
        static_count = result_type->object.field_count;
        key_names = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(const char *) * (size_t) static_count));
        if (!key_names)
            return NULL;
        for (int i = 0; i < static_count; i++) {
            const char *nm = result_type->object.field_names[i];
            key_names[i] = nm ? arena_strdup(l->func, nm) : "?";
        }
    }

    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_JSON_NEW, result_type, 0);
    if (!obj_val)
        return NULL;
    obj_val->aux_int = static_count;
    obj_val->aux = (void *) key_names;
    obj_val->line = (uint32_t) node->line;

    for (int i = 0; i < obj->count; i++) {
        AstNode *val = obj->values[i];
        if (val && val->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, val->as.spread_expr.expr);
            if (!src)
                return NULL;
            XiValue *mg = xi_value_new(l->func, l->cur_block, XI_JSON_MERGE, l->type_unit, 2);
            if (!mg)
                return NULL;
            mg->args[0] = obj_val;
            mg->args[1] = src;
            mg->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
            mg->line = (uint32_t) node->line;
            continue;
        }

        XiValue *v = xi_lower_expr(l, obj->values[i]);
        if (!v)
            return NULL;
        bool is_computed = obj->computed && obj->computed[i];

        /* Static literal key: write the field by its index in the union shape
         * (XI_JSON_SET_F), matching how spread merges and member reads address
         * the same slots. Computed keys are not part of the static shape and
         * fall back to a dynamic key set. */
        int field_idx = -1;
        if (!is_computed && obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING &&
            static_count > 0 && key_names) {
            const char *kn = obj->keys[i]->as.literal.raw_value.string_val;
            for (int k = 0; kn && k < static_count; k++) {
                if (key_names[k] && strcmp(key_names[k], kn) == 0) {
                    field_idx = k;
                    break;
                }
            }
        }

        if (field_idx >= 0) {
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, l->type_unit, 2);
            if (!set)
                return NULL;
            set->args[0] = obj_val;
            set->args[1] = v;
            set->aux_int = field_idx;
            set->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            set->line = (uint32_t) node->line;
        } else {
            XiValue *key = is_computed ? xi_lower_expr(l, obj->keys[i])
                                       : xi_const_str(l->func, l->cur_block, "?", l->type_string);
            if (!key)
                return NULL;
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
            if (!set)
                return NULL;
            set->args[0] = obj_val;
            set->args[1] = key;
            set->args[2] = v;
            set->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            set->line = (uint32_t) node->line;
        }
    }
    return obj_val;
}

XR_FUNC XiValue *xi_lower_object_literal(XiLower *l, AstNode *node) {
    ObjectLiteralNode *obj = &node->as.object_literal;

    /* Spread entries force the dynamic merge path. */
    for (int i = 0; i < obj->count; i++) {
        if (obj->values[i] && obj->values[i]->type == AST_SPREAD_EXPR)
            return xi_lower_object_literal_spread(l, node, xi_lower_node_type(l, node));
    }

    int count = obj->count;
    if (count < 0 || count > (int) UINT16_MAX) {
        fprintf(stderr, "[LOWER] object literal field count exceeds %u at line %d\n",
                (unsigned) UINT16_MAX, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    int n = count;

    /* Evaluate all values and computed key expressions first */
    int alloc_n = n > 0 ? n : 1;
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    XiValue **key_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    int *static_idx_map =
        (int *) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(int)));
    if (!val_vals || !key_vals || !static_idx_map)
        return NULL;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, obj->values[i]);
        if (!val_vals[i])
            return NULL;
        bool is_computed = obj->computed && obj->computed[i];
        key_vals[i] = is_computed ? xi_lower_expr(l, obj->keys[i]) : NULL;
        if (is_computed && !key_vals[i])
            return NULL;
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

/* ========== Error Propagation ========== */

XR_FUNC void xi_lower_insert_err_check(XiLower *l, struct AstNode *node) {
    if (!l->cur_block)
        return;

    if (l->try_depth > 0) {
        /* Inside try body: check error channel and jump to the catch target
         * if an error is pending.  Conditional branch — the non-error path
         * continues normally. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_ERR_CHECK, l->type_bool, 0);
        if (!check)
            return;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;

        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        XiBlock *err_blk = xi_block_new(l->func);
        XiBlock *cont = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, check, err_blk, cont);
        xi_lower_braun_seal(l, err_blk);
        l->cur_block = err_blk;
        xi_lower_defer_run_to_depth(l, l->catch_defer_depths[l->try_depth - 1],
                                    node ? node->line : 0);
        xi_block_set_jump(l->cur_block, catch_blk);

        xi_lower_braun_seal(l, cont);
        l->cur_block = cont;
    } else {
        /* Outside try: unconditional propagation via OP_ERR_CHECK. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_ERR_CHECK, l->type_unit, 0);
        if (!check)
            return;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;
    }
}
