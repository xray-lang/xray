/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_misc.c - Miscellaneous expression lowering helpers
 *
 * Contains: enum access/decl, object literal, catch expr,
 * cancelled/move lowering.  Extracted from xi_lower_expr.c to keep
 * individual translation units within the 3000-line limit.
 */

#include "xi_lower_internal.h"
#include "xi.h"
#include "xi_effect.h"
#include "../analysis/xglobal_summary.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../shared/xobject_shape.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
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
    if (strcmp(enum_name, "Endian") == 0)
        return XR_GLOBAL_VAR_ENDIAN;
    if (strcmp(enum_name, "Recv") == 0)
        return XR_GLOBAL_VAR_RECV;
    if (strcmp(enum_name, "SendResult") == 0)
        return XR_GLOBAL_VAR_SEND_RESULT;
    if (strcmp(enum_name, "TaskResult") == 0)
        return XR_GLOBAL_VAR_TASK_RESULT;
    if (strcmp(enum_name, "TaskStatus") == 0)
        return XR_GLOBAL_VAR_TASK_STATUS;
    if (strcmp(enum_name, "Utf8Error") == 0)
        return XR_GLOBAL_VAR_UTF8_ERROR;
    if (strcmp(enum_name, "StringSliceError") == 0)
        return XR_GLOBAL_VAR_STRING_SLICE_ERROR;
    if (strcmp(enum_name, "CompressionError") == 0)
        return XR_GLOBAL_VAR_COMPRESSION_ERROR;
    if (strcmp(enum_name, "CryptoError") == 0)
        return XR_GLOBAL_VAR_CRYPTO_ERROR;
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
        XiFunc *mf = xi_lower_method_as_func(l, md, !md->is_static, NULL, false, receiver_type,
                                             xi_lower_source_node_id(l, method));
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

/* ========== Enum Declaration ========== */

static uint32_t xi_lower_decl_derive_flags(XrAttribute **attrs, int count) {
    uint32_t flags = 0;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DERIVE)
            flags |= attrs[i]->derive_flags;
    }
    return flags;
}

static int xi_lower_configure_adt_layout(XiLower *l, XrEnumType *enum_type,
                                         const EnumDeclNode *decl,
                                         const XiEnumMemberData *members) {
    int variant_count = decl->member_count;
    int max_payload = 0;
    int *payload_counts = (int *) xr_calloc((size_t) variant_count, sizeof(int));
    if (payload_counts) {
        for (int i = 0; i < variant_count; i++) {
            int count = decl->members[i]->as.enum_member.payload_count;
            payload_counts[i] = count;
            if (count > max_payload)
                max_payload = count;
        }
        (void) xr_enum_type_set_adt_payloads(enum_type, payload_counts, variant_count);
        xr_free(payload_counts);
    }
    if (!enum_type->layout)
        return max_payload;

    for (int i = 0; i < variant_count; i++) {
        EnumMemberNode *member = &decl->members[i]->as.enum_member;
        if (member->payload_count <= 0)
            continue;
        uint8_t *type_ids =
            (uint8_t *) xr_calloc((size_t) member->payload_count, sizeof(*type_ids));
        if (!type_ids)
            continue;
        for (int p = 0; p < member->payload_count; p++) {
            XrType *payload_type =
                members && members[i].payload_types ? members[i].payload_types[p] : NULL;
            type_ids[p] = xr_type_to_tid(payload_type);
        }
        (void) xr_enum_layout_set_variant_payload_metadata(
            enum_type->layout, (uint32_t) i, (const char *const *) member->payload_names, type_ids,
            (uint16_t) member->payload_count);
        xr_free(type_ids);
    }
    (void) l;
    return max_payload;
}

/* Lower AST_ENUM_DECL: create XrEnumType at compile time, store as
 * shared variable so enum member access can find it.
 * Handles both zero-payload enums and payload enums. The temporary runtime
 * object model stores only declaration-order tags and cold metadata. */
XR_FUNC void xi_lower_enum_decl(XiLower *l, AstNode *node) {
    EnumDeclNode *ed = &node->as.enum_decl;
    XR_DCHECK(ed->name != NULL, "enum name must not be NULL");
    XR_DCHECK(l->isolate != NULL, "isolate required for enum creation");

    int n = ed->member_count;
    char **names = (char **) xr_malloc(sizeof(char *) * (size_t) n);
    if (!names) {
        xr_free(names);
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
            if (ed->type_param_count > 0 && ed->type_params) {
                const char **type_param_names = (const char **) xi_func_arena_alloc(
                    l->func, (uint32_t) ((size_t) ed->type_param_count * sizeof(const char *)));
                if (type_param_names) {
                    for (int tp = 0; tp < ed->type_param_count; tp++) {
                        type_param_names[tp] = arena_strdup(
                            l->func, ed->type_params[tp] ? ed->type_params[tp]->name : NULL);
                    }
                    enum_data->type_param_names = type_param_names;
                    enum_data->type_param_count = (uint8_t) ed->type_param_count;
                }
            }
        } else {
            enum_data = NULL;
        }
    }
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        names[i] = xr_strdup(m->name);
        if (enum_members) {
            enum_members[i].name = arena_strdup(l->func, m->name);
            enum_members[i].ordinal = (uint32_t) i;
            enum_members[i].payload_count = m->payload_count;
            if (m->payload_count > 0 && m->payload_names) {
                const char **payload_names = (const char **) xi_func_arena_alloc(
                    l->func, (uint32_t) ((size_t) m->payload_count * sizeof(const char *)));
                if (payload_names) {
                    for (int p = 0; p < m->payload_count; p++)
                        payload_names[p] =
                            arena_strdup(l->func, m->payload_names[p] ? m->payload_names[p] : "");
                    enum_members[i].payload_names = payload_names;
                }
            }
            if (m->payload_count > 0 && m->payload_types) {
                XrType **payload_types = (XrType **) xi_func_arena_alloc(
                    l->func, (uint32_t) ((size_t) m->payload_count * sizeof(XrType *)));
                if (payload_types) {
                    memset(payload_types, 0, (size_t) m->payload_count * sizeof(XrType *));
                    for (int p = 0; p < m->payload_count; p++) {
                        payload_types[p] =
                            xr_tref_resolve_in_analyzer(l->analyzer, m->payload_types[p]);
                        payload_types[p] =
                            xi_lower_type_or_any(l, payload_types[p], "enum payload type", 0);
                    }
                    enum_members[i].payload_types = payload_types;
                }
            }
        }
    }

    XaSymbol *enum_symbol =
        ed->symbol_id ? xa_scope_lookup_by_id(l->analyzer->global_scope, ed->symbol_id) : NULL;
    XaSymbolLinks *enum_links =
        enum_symbol ? xa_analyzer_get_links(l->analyzer, enum_symbol) : NULL;
    const char *nominal_owner =
        enum_links && enum_links->enum_info ? enum_links->enum_info->nominal_owner : NULL;
    XrEnumType *et =
        nominal_owner ? xr_enum_type_new(l->isolate, nominal_owner, ed->name, names, n) : NULL;
    if (!et)
        l->had_error = true;
    if (et)
        et->derive_flags = xi_lower_decl_derive_flags(ed->attributes, ed->attr_count);
    for (int i = 0; i < n; i++)
        xr_free(names[i]);
    xr_free(names);

    /* Set tagged aggregate layout metadata on the created enum type. */
    if (et && is_adt && enum_data)
        enum_data->max_payload = xi_lower_configure_adt_layout(l, et, ed, enum_members);
    else if (et && is_adt)
        (void) xi_lower_configure_adt_layout(l, et, ed, enum_members);
    if (enum_data) {
        enum_data->layout_id = et && et->layout ? et->layout->layout_id : 0;
        enum_data->runtime_type = et;
    }

    /* Store the detached enum descriptor, never the compiler-isolate runtime
     * pointer. VM and AOT both consume the same XiEnumData contract. */
    XiValue *cv = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
    if (!cv)
        return;
    cv->aux = enum_data;
    cv->aux_kind = XI_AUX_KIND_ENUM_NAMESPACE;
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
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SOURCE_MOVE, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = val;
    v->line = (uint32_t) node->line;
    if (me->expr && me->expr->type == AST_VARIABLE && me->expr->as.variable.symbol_id != 0 &&
        l->analyzer) {
        XaSymbol *symbol =
            xa_scope_lookup_by_id(l->analyzer->global_scope, me->expr->as.variable.symbol_id);
        XaSymbolLinks *links = symbol ? xa_analyzer_get_links(l->analyzer, symbol) : NULL;
        if (links && links->ownership_candidate.complete) {
            v->move_evidence_id = links->ownership_candidate.id;
            v->move_source_root_id = links->ownership_candidate.root;
            v->move_source_symbol_id = links->ownership_candidate.source_symbol_id;
            v->move_storage_plan_id = links->allocation_plan.id;
            v->move_evidence_bits = links->ownership_candidate.evidence | XA_OWNERSHIP_EV_STORAGE;
            v->move_source_capability = (uint8_t) links->value_capability;
            v->move_target_capability = (uint8_t) links->value_capability;
            v->move_source_domain = (uint8_t) links->allocation_plan.domain;
            v->move_target_domain = (uint8_t) links->allocation_plan.domain;
        }
    }
    return v;
}

/* ========== Object Literal ========== */

static const XgObjectFieldSummary *xi_lower_object_shape_field_at(const XgGlobalEvidence *evidence,
                                                                  XgObjectShapeId shape_id,
                                                                  uint16_t ordinal) {
    if (!evidence || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &evidence->object_fields[i];
        if (field->shape_id == shape_id && field->field_ordinal == ordinal)
            return field;
    }
    return NULL;
}

static bool xi_lower_fill_object_field_names_from_evidence(XiLower *l, const XrType *type,
                                                           const char **names, int count,
                                                           uint32_t source_span_id) {
    if (!l || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID || !type ||
        !type->object.field_names || count <= 0 || count > (int) UINT16_MAX || source_span_id == 0)
        return false;
    const XgGlobalEvidence *evidence = l->global_evidence;
    uint8_t domain = XG_OBJECT_DOMAIN_STRUCT;
    for (uint32_t i = 0; i < evidence->nobject_shapes; i++) {
        const XgObjectShapeSummary *shape = &evidence->object_shapes[i];
        if (shape->owner_func_id != (XgFuncId) l->func->xg_body_func_id ||
            shape->source_span_id != source_span_id || shape->field_count != (uint16_t) count ||
            shape->domain != domain)
            continue;
        bool matches = true;
        for (int ordinal = 0; ordinal < count; ordinal++) {
            const XgObjectFieldSummary *field = xi_lower_object_shape_field_at(
                evidence, shape->object_shape_id, (uint16_t) ordinal);
            const char *matched_name = NULL;
            if (!field) {
                matches = false;
                break;
            }
            for (int type_ordinal = 0; type_ordinal < count; type_ordinal++) {
                const char *candidate = type->object.field_names[type_ordinal];
                if (candidate && xg_name_id(candidate) == field->name_id &&
                    xg_object_stable_name_key(candidate) == field->stable_name_key) {
                    matched_name = candidate;
                    break;
                }
            }
            if (!matched_name) {
                matches = false;
                break;
            }
            names[ordinal] = matched_name;
        }
        if (!matches)
            continue;
        for (int ordinal = 0; ordinal < count; ordinal++) {
            names[ordinal] = arena_strdup(l->func, names[ordinal]);
            if (!names[ordinal])
                return false;
        }
        return true;
    }
    return false;
}

XR_FUNC bool xi_lower_fill_canonical_object_field_names(XiLower *l, const XrType *type,
                                                        const char **names, int count,
                                                        uint32_t source_span_id) {
    if (!l || !type || !names || count <= 0 || !XR_TYPE_HAS_OBJECT_SHAPE(type) ||
        type->object.field_count != count || !type->object.field_names)
        return false;
    if (xi_lower_fill_object_field_names_from_evidence(l, type, names, count, source_span_id))
        return true;
    for (int i = 0; i < count; i++) {
        const char *name = type->object.field_names[i];
        names[i] = arena_strdup(l->func, name ? name : "?");
        if (!names[i])
            return false;
    }
    /* Only a struct object sorts. Its shape is nominal, so the global producer
     * canonicalizes its keys by stable name key and this fallback has to land
     * on the same order the evidence path above would have given it. A Json
     * literal is positional: the producer numbers its fields as the source
     * wrote them, and that order is what printing, iteration, and every
     * ordinal read observe. Sorting one like the other gives the two backends
     * different layouts for the same literal -- the field a Json ordinal names
     * then depends on a name hash rather than on the source. */
    if (type->kind != XR_KIND_STRUCT_OBJECT)
        return true;
    for (int i = 1; i < count; i++) {
        const char *current = names[i];
        uint64_t current_stable = xg_object_stable_name_key(current);
        uint32_t current_id = xg_name_id(current);
        int j = i;
        while (j > 0) {
            uint64_t previous_stable = xg_object_stable_name_key(names[j - 1]);
            uint32_t previous_id = xg_name_id(names[j - 1]);
            if (previous_stable < current_stable ||
                (previous_stable == current_stable && previous_id <= current_id))
                break;
            names[j] = names[j - 1];
            j--;
        }
        names[j] = current;
    }
    return true;
}

/* Object literal with `...spread` entries: `{...base, x: 1}`.
 * The result object is created pre-sized with the exact, statically-known union shape
 * (from the analyzer's inferred type), then each part is applied in order:
 * spread sources are merged field-by-field (XI_OBJECT_MERGE), literal fields are
 * written by key (XI_INDEX_SET). Later writes override earlier ones, matching
 * the union semantics. */
static XiValue *xi_lower_object_literal_spread(XiLower *l, AstNode *node,
                                               struct XrType *result_type) {
    ObjectLiteralNode *obj = &node->as.object_literal;

    /* Pre-size the result with the union of statically-known field names. */
    int static_count = 0;
    const char **key_names = NULL;
    if (XR_TYPE_HAS_OBJECT_SHAPE(result_type) && result_type->object.field_count > 0 &&
        result_type->object.field_names) {
        static_count = result_type->object.field_count;
        key_names = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(const char *) * (size_t) static_count));
        if (!key_names)
            return NULL;
        if (!xi_lower_fill_canonical_object_field_names(l, result_type, key_names, static_count,
                                                        (uint32_t) node->line))
            return NULL;
    }

    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_OBJECT_NEW, result_type, 0);
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
            XiValue *mg = xi_value_new(l->func, l->cur_block, XI_OBJECT_MERGE, l->type_unit, 2);
            if (!mg)
                return NULL;
            mg->args[0] = obj_val;
            mg->args[1] = src;
            mg->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
            mg->line = (uint32_t) val->line;
            xi_lower_bind_object_merge_id(l, mg,
                                          xi_lower_source_node_id(l, val->as.spread_expr.expr));
            continue;
        }

        XiValue *v = xi_lower_expr(l, obj->values[i]);
        if (!v)
            return NULL;
        /* Static literal key: write the field by its index in the union shape
         * (XI_OBJECT_SET_F), matching how spread merges and member reads address
         * the same slots. */
        int field_idx = -1;
        if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING && static_count > 0 &&
            key_names) {
            const char *kn = obj->keys[i]->as.literal.raw_value.string_val;
            for (int k = 0; kn && k < static_count; k++) {
                if (key_names[k] && strcmp(key_names[k], kn) == 0) {
                    field_idx = k;
                    break;
                }
            }
        }

        if (field_idx >= 0) {
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_OBJECT_SET_F, l->type_unit, 2);
            if (!set)
                return NULL;
            set->args[0] = obj_val;
            set->args[1] = v;
            set->aux_int = field_idx;
            set->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            set->lowering_flags |= XI_LOWERING_FLAG_OBJECT_LITERAL_INIT;
            set->line = (uint32_t) node->line;
        } else {
            fprintf(stderr, "[LOWER] structural object literal has no static field ordinal\n");
            l->had_error = true;
            return NULL;
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

    /* Evaluate all values first. Structural object keys are always static. */
    int alloc_n = n > 0 ? n : 1;
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    int *static_idx_map =
        (int *) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(int)));
    if (!val_vals || !static_idx_map)
        return NULL;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, obj->values[i]);
        if (!val_vals[i])
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    bool canonical_object_shape = result_type && XR_TYPE_HAS_OBJECT_SHAPE(result_type) &&
                                  result_type->object.field_count > 0 &&
                                  result_type->object.field_names;

    /* A contextual structural object must retain its declared shape even when optional
     * fields are omitted. Otherwise a later supplied field is compacted into
     * an earlier optional slot and direct field reads observe the wrong value. */
    int static_count = 0;
    if (canonical_object_shape) {
        static_count = result_type->object.field_count;
    } else {
        static_count = n;
    }

    /* Collect static key names (arena-allocated) */
    const char **key_names = (const char **) xi_func_arena_alloc(
        l->func, (uint32_t) (sizeof(const char *) * (static_count > 0 ? static_count : 1)));
    if (!key_names)
        return NULL;
    if (canonical_object_shape) {
        if (!xi_lower_fill_canonical_object_field_names(l, result_type, key_names, static_count,
                                                        (uint32_t) node->line))
            return NULL;
        for (int i = 0; i < n; i++) {
            static_idx_map[i] = -1;
            if (!obj->keys[i] || obj->keys[i]->type != AST_LITERAL_STRING)
                continue;
            const char *literal_name = obj->keys[i]->as.literal.raw_value.string_val;
            for (int k = 0; literal_name && k < static_count; k++) {
                if (key_names[k] && strcmp(key_names[k], literal_name) == 0) {
                    static_idx_map[i] = k;
                    break;
                }
            }
        }
    } else {
        int si = 0;
        for (int i = 0; i < n; i++) {
            if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING) {
                const char *name = obj->keys[i]->as.literal.raw_value.string_val;
                key_names[si] = arena_strdup(l->func, name ? name : "?");
            } else {
                key_names[si] = arena_strdup(l->func, "?");
            }
            if (!key_names[si])
                return NULL;
            static_idx_map[i] = si;
            si++;
        }
    }

    /* Create a structural object with an exact static-key shape. */
    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_OBJECT_NEW, result_type, 0);
    if (!obj_val)
        return NULL;
    obj_val->aux_int = static_count;
    obj_val->aux = (void *) key_names;
    obj_val->line = (uint32_t) node->line;

    /* Initialize every structural field by its canonical index. */
    for (int i = 0; i < n; i++) {
        if (static_idx_map[i] < 0)
            return NULL;
        XiValue *init = xi_value_new(l->func, l->cur_block, XI_OBJECT_INIT_F, l->type_unit, 2);
        if (!init)
            return NULL;
        init->args[0] = obj_val;
        init->args[1] = val_vals[i];
        init->aux_int = static_idx_map[i];
        init->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return obj_val;
}

/* ========== Error Propagation ========== */

XR_FUNC void xi_lower_insert_err_check(XiLower *l, struct AstNode *node, bool producer_may_throw) {
    if (!l->cur_block)
        return;

    /* Constructive generation (task 216): a producer proven NO_THROW can never
     * leave a pending error, so the check is not emitted at all. This replaces
     * the historical "insert unconditionally, delete later by evidence" path. */
    if (!producer_may_throw)
        return;

    if (l->cleanup_body_depth > 0 && l->try_depth <= l->cleanup_body_try_base_depth) {
        /* Cleanup bodies are closed error regions. The analyzer proves that
         * no value error escapes; retain a fail-closed runtime check for an
         * opaque native/indirect call without recursively entering the same
         * cleanup frontier. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_CLEANUP_ERR_CHECK, l->type_unit, 0);
        if (!check)
            return;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;
    } else if (l->try_depth > 0) {
        /* Inside try body: check error channel and jump to the catch target
         * if an error is pending.  Conditional branch — the non-error path
         * continues normally. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_ERR_CHECK, l->type_bool, 0);
        if (!check)
            return;
        check->error_region = l->active_error_region;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;

        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        XiBlock *err_blk = xi_block_new(l->func);
        XiBlock *cont = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, check, err_blk, cont);
        xi_lower_braun_seal(l, err_blk);
        l->cur_block = err_blk;
        xi_lower_cleanup_run_to_depth(l, l->catch_cleanup_depths[l->try_depth - 1],
                                      node ? node->line : 0);
        xi_block_set_jump(l->cur_block, catch_blk);

        xi_lower_braun_seal(l, cont);
        l->cur_block = cont;
    } else if (!xi_lower_cleanup_has_active_site(l)) {
        /* Outside a cleanup owner, use the compact direct propagation form. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_ERR_CHECK, l->type_unit, 0);
        if (!check)
            return;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;
    } else {
        /* An escaping value error is an ordinary CFG edge. Materialize it so
         * the same static cleanup ladder used by return/break can run first. */
        XiValue *check = xi_value_new(l->func, l->cur_block, XI_ERR_CHECK, l->type_bool, 0);
        if (!check)
            return;
        check->flags |= XI_FLAG_SIDE_EFFECT;
        check->line = node ? (uint32_t) node->line : 0;
        XiBlock *err_block = xi_block_new(l->func);
        XiBlock *cont = xi_block_new(l->func);
        xi_block_set_if(l->cur_block, check, err_block, cont);

        xi_lower_braun_seal(l, err_block);
        l->cur_block = err_block;
        XiValue *error = xi_value_new(
            l->func, l->cur_block, XI_ERR_CATCH,
            xi_lower_type_or_any(l, NULL, "cleanup value-error propagation", node ? node->line : 0),
            0);
        if (error) {
            error->flags |= XI_FLAG_SIDE_EFFECT;
            error->line = node ? (uint32_t) node->line : 0;
        }
        xi_lower_cleanup_run_to_depth(l, 0, node ? node->line : 0);
        if (l->cur_block && error) {
            XiValue *reprop = xi_value_new(l->func, l->cur_block, XI_ERR_RETURN, l->type_unit, 1);
            if (reprop) {
                reprop->args[0] = error;
                reprop->flags |= XI_FLAG_SIDE_EFFECT;
                reprop->line = node ? (uint32_t) node->line : 0;
            }
            l->cur_block->kind = XI_BLOCK_RETURN;
            l->cur_block->control = reprop;
        }
        xi_lower_braun_seal(l, cont);
        l->cur_block = cont;
    }
}
