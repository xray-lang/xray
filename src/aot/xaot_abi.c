/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_abi.c - AOT function ABI plan
 */

#include "xaot_abi.h"
#include "xaot_bundle.h"
#include "xaot_class_native.h"
#include "xaot_abi_gen.h"
#include "xaot_layout_gen.h"
#include "xaot_struct_name.h"
#include "../ir/xi_ops_gen.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_info.h"
#include <string.h>

static bool func_has_op_class(const XiFunc *func, uint8_t op_class) {
    uint32_t bi;

    if (!func)
        return false;
    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && xi_generated_op_class(v->op) == op_class)
                return true;
        }
    }
    return false;
}

static XaotArgClass arg_class_for_value_rep(XaotValueRep rep) {
    switch (rep.kind) {
        case XAOT_VALUE_VOID:
            return XAOT_ARG_VOID;
        case XAOT_VALUE_SCALAR:
            return XAOT_ARG_SCALAR;
        case XAOT_VALUE_PTR:
            return XAOT_ARG_PTR;
        case XAOT_VALUE_AGGREGATE:
            return XAOT_ARG_AGG_BY_VALUE;
        case XAOT_VALUE_TAGGED:
        default:
            return XAOT_ARG_TAGGED;
    }
}

static XaotAbiSlot tagged_slot(const XrType *type) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.cls = XAOT_ARG_TAGGED;
    slot.rep.kind = XAOT_VALUE_TAGGED;
    slot.rep.rep = XAOT_REP_TAGGED;
    slot.rep.type = type;
    slot.rep.c_type = "XrValue";
    slot.c_type = "XrValue";
    return slot;
}

static bool type_is_class_instance_ptr_boundary(const XaotBundle *bundle, const XrType *type) {
    return type && !type->is_nullable &&
           (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
           type->instance.class_name != NULL && xaot_class_native_data_for_type(bundle, type);
}

static XaotValueRep ptr_value_rep_for_type(const XrType *type) {
    XaotValueRep rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = XAOT_VALUE_PTR;
    rep.rep = XAOT_REP_PTR;
    rep.type = type;
    rep.c_type = "void *";
    return rep;
}

static XaotAbiSlot ptr_slot(const XrType *type) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.cls = XAOT_ARG_PTR;
    slot.rep = ptr_value_rep_for_type(type);
    slot.c_type = slot.rep.c_type;
    return slot;
}

static bool bundle_is_freestanding_profile(const XaotBundle *bundle) {
    return bundle && bundle->global_evidence_plan.profile == XG_BUILD_FREESTANDING;
}

static bool type_is_freestanding_ordinal_enum(const XaotBundle *bundle, const XrType *type) {
    return bundle_is_freestanding_profile(bundle) && type && !type->is_nullable &&
           type->kind == XR_KIND_ENUM && type->enum_type.layout &&
           type->enum_type.layout->is_zero_payload;
}

static XaotValueRep enum_ordinal_value_rep(const XrType *type) {
    XaotValueRep rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = XAOT_VALUE_SCALAR;
    rep.rep = XAOT_REP_I64;
    rep.type = type;
    rep.c_type = "int64_t";
    rep.flags = XAOT_VALUE_FLAG_ENUM;
    return rep;
}

static XaotAbiSlot enum_ordinal_slot(const XrType *type) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.cls = XAOT_ARG_SCALAR;
    slot.rep = enum_ordinal_value_rep(type);
    slot.c_type = slot.rep.c_type;
    return slot;
}

static bool fixed_array_lane_native_info(const XrType *type, uint8_t *native_out,
                                         const char **ctype_out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0)
        return false;
    XrType *elem = type->fixed_array.element_type;
    int native = xr_type_kind_to_native(elem->kind, elem->native_width);
    if (elem->is_nullable || native == XR_NATIVE_STRING || native < 0)
        native = XR_NATIVE_VALUE;
    const char *ctype = xaot_layout_c_type_for_native_type((uint8_t) native);
    if (!ctype)
        return false;
    if (native_out)
        *native_out = (uint8_t) native;
    if (ctype_out)
        *ctype_out = ctype;
    return true;
}

static bool param_uses_place_abi(const XiFunc *func, const XiValue *value, bool is_return,
                                 XrParamMode *mode_out) {
    if (is_return || !func || !value || value->op != XI_PARAM || value->aux_int < 0)
        return false;
    uint16_t index = (uint16_t) value->aux_int;
    XrParamMode mode = xi_func_param_passing_mode(func, index);
    if (mode == XR_PARAM_VALUE)
        return false;
    if (mode_out)
        *mode_out = mode;
    return true;
}

static bool fixed_array_place_param_can_use_ptr_abi(const XiFunc *func, const XiValue *value,
                                                    const XrType *type, bool is_return,
                                                    const char **elem_ctype_out) {
    if (!param_uses_place_abi(func, value, is_return, NULL) || !value || value->op != XI_PARAM ||
        value->aux_int < 0 || !type || type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    return fixed_array_lane_native_info(type, NULL, elem_ctype_out);
}

static char *fixed_array_ref_param_c_type(const char *elem_ctype) {
    if (!elem_ctype)
        return NULL;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s *", elem_ctype);
    if (n <= 0 || (size_t) n >= sizeof(buf))
        return NULL;
    return xr_strdup(buf);
}

static XaotAbiSlot fixed_array_place_param_slot(const XrType *type, const char *elem_ctype) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    char *c_type = fixed_array_ref_param_c_type(elem_ctype);
    if (!c_type)
        return tagged_slot(type);
    slot.cls = XAOT_ARG_PTR;
    slot.rep.kind = XAOT_VALUE_VIEW;
    slot.rep.rep = XAOT_REP_RAWPTR;
    slot.rep.type = type;
    slot.rep.c_type = c_type;
    slot.rep.flags = XAOT_VALUE_FLAG_OWNED_C_TYPE;
    slot.pointee_rep.kind = XAOT_VALUE_VIEW;
    slot.pointee_rep.rep = XAOT_REP_RAWPTR;
    slot.pointee_rep.type = type;
    slot.pointee_rep.c_type = "void *";
    slot.c_type = slot.rep.c_type;
    slot.flags |= XAOT_ABI_SLOT_BORROWED_PLACE;
    return slot;
}

static bool abi_type_can_use_typed_boundary(const XaotBundle *bundle, const XrType *type) {
    return xaot_abi_type_can_use_typed_boundary(type) ||
           type_is_class_instance_ptr_boundary(bundle, type);
}

static const XaotEnumPlan *adt_enum_plan_for_type(const XaotBundle *bundle, const XrType *type) {
    const XaotEnumPlan *plan = xaot_bundle_find_enum_plan_for_type(bundle, type);
    return (plan && plan->scalar_action == XAOT_ENUM_SCALAR_COMPACT_AGGREGATE) ? plan : NULL;
}

static bool type_can_use_compact_adt_return(const XaotBundle *bundle, const XrType *type) {
    return adt_enum_plan_for_type(bundle, type) != NULL;
}

static bool func_tree_contains(const XiFunc *root, const XiFunc *target) {
    if (!root || !target)
        return false;
    if (root == target)
        return true;
    for (uint16_t i = 0; i < root->nchildren; i++) {
        if (func_tree_contains(root->children ? root->children[i] : NULL, target))
            return true;
    }
    return false;
}

static const char *func_module_prefix(const XaotBundle *bundle, const XiFunc *func) {
    for (const XiFunc *cur = func; cur; cur = cur->parent_func) {
        if (cur->module && cur->module->name && cur->module->name[0])
            return cur->module->name;
    }
    if (bundle) {
        for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
            const XiModule *mod = bundle->modules ? bundle->modules[mi] : NULL;
            if (mod && mod->name && mod->name[0] && func_tree_contains(mod->init, func))
                return mod->name;
        }
    }
    return "mod";
}

static const XrAggregateLayout *struct_layout_for_type(const XaotBundle *bundle,
                                                       const XrType *type) {
    const char *name;
    if (!type || type->is_nullable ||
        (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE))
        return NULL;
    if (type->instance.class_ref && type->instance.class_ref->struct_layout)
        return type->instance.class_ref->struct_layout;
    name = type->instance.class_name;
    if (!bundle || !name)
        return NULL;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules ? bundle->modules[mi] : NULL;
        if (!mod || !mod->classes)
            continue;
        for (uint16_t ci = 0; ci < mod->nclasses; ci++) {
            const XiClassData *cd = mod->classes[ci];
            if (cd && cd->class_name && cd->struct_layout && strcmp(cd->class_name, name) == 0)
                return cd->struct_layout;
        }
    }
    return NULL;
}

static const XiValue *unwrap_identity_value(const XiValue *v) {
    while (v &&
           (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_RETAIN || v->op == XI_BOX ||
            v->op == XI_UNBOX) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static const XrAggregateLayout *struct_layout_for_value_uses(const XiFunc *func,
                                                             const XiValue *value) {
    value = unwrap_identity_value(value);
    if (!func || !value)
        return NULL;
    if (value->op == XI_AGG_NEW && value->aux)
        return (const XrAggregateLayout *) value->aux;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values ? blk->values[vi] : NULL;
            if (!v || !v->aux || v->nargs < 1)
                continue;
            if ((v->op == XI_AGG_GET || v->op == XI_AGG_SET) &&
                unwrap_identity_value(v->args[0]) == value)
                return (const XrAggregateLayout *) v->aux;
        }
    }
    return NULL;
}

static const XrAggregateLayout *struct_layout_for_return_value(const XiFunc *func) {
    if (!func)
        return NULL;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk || blk->kind != XI_BLOCK_RETURN || !blk->control)
            continue;
        const XrAggregateLayout *sl = struct_layout_for_value_uses(func, blk->control);
        if (sl)
            return sl;
    }
    return NULL;
}

static const XrAggregateLayout *struct_layout_for_slot(const XaotBundle *bundle, const XiFunc *func,
                                                       const XrType *type, const XiValue *value,
                                                       bool is_return) {
    const XrAggregateLayout *sl = struct_layout_for_type(bundle, type);
    if (sl)
        return sl;
    if (is_return)
        return struct_layout_for_return_value(func);
    return struct_layout_for_value_uses(func, value);
}

static bool struct_layout_can_use_value_abi_depth(const XrAggregateLayout *sl, int depth) {
    if (!sl || sl->field_count == 0 || sl->field_count > XR_MAX_AGG_FIELDS || depth > 8)
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        const XrAggregateFieldLayout *field = &sl->fields[i];
        const XaotLayoutInfo *info = xaot_layout_for_native_type(field->native_type);
        if (!info)
            return false;
        if (info->field_kind == XAOT_LAYOUT_FIELD_SCALAR)
            continue;
        if (info->field_kind == XAOT_LAYOUT_FIELD_INLINE_ARRAY) {
            const XaotLayoutInfo *elem = xaot_layout_for_native_type(field->elem_native_type);
            if (!elem || elem->field_kind != XAOT_LAYOUT_FIELD_SCALAR || field->elem_count == 0)
                return false;
            continue;
        }
        if (info->field_kind == XAOT_LAYOUT_FIELD_NESTED_AGGREGATE) {
            if (!struct_layout_can_use_value_abi_depth(field->sub_layout, depth + 1))
                return false;
            continue;
        }
        return false;
    }
    return true;
}

static char *struct_c_type_for_func(const XaotBundle *bundle, const XiFunc *func,
                                    const XrAggregateLayout *sl) {
    char buf[128];
    xaot_struct_c_type_name(buf, sizeof(buf), func_module_prefix(bundle, func), sl);
    return xr_strdup(buf);
}

static XaotValueRep struct_value_rep_for_slot(const XaotBundle *bundle, const XiFunc *func,
                                              const XrType *type, const XiValue *value,
                                              bool is_return) {
    XaotValueRep rep;
    const XrAggregateLayout *sl = struct_layout_for_slot(bundle, func, type, value, is_return);

    memset(&rep, 0, sizeof(rep));
    if (!struct_layout_can_use_value_abi_depth(sl, 0))
        return rep;
    rep.kind = XAOT_VALUE_AGGREGATE;
    rep.rep = XAOT_REP_TAGGED;
    rep.type = type;
    rep.c_type = struct_c_type_for_func(bundle, func, sl);
    rep.flags = XAOT_VALUE_FLAG_STRUCT | XAOT_VALUE_FLAG_OWNED_C_TYPE;
    return rep;
}

static XaotValueRep compact_adt_value_rep_for_type(const XaotBundle *bundle, const XrType *type) {
    XaotValueRep rep;
    const XaotEnumPlan *plan = adt_enum_plan_for_type(bundle, type);

    memset(&rep, 0, sizeof(rep));
    rep.kind = XAOT_VALUE_AGGREGATE;
    rep.rep = XAOT_REP_TAGGED;
    rep.type = type;
    rep.c_type = plan && plan->c_type ? plan->c_type : "XrAotEnumAggregate";
    rep.flags = XAOT_VALUE_FLAG_ENUM | XAOT_VALUE_FLAG_ENUM_AGGREGATE;
    return rep;
}

static XaotAbiSlot compact_adt_return_slot(const XaotBundle *bundle, const XrType *type) {
    XaotAbiSlot slot;

    memset(&slot, 0, sizeof(slot));
    slot.cls = XAOT_ARG_AGG_BY_VALUE;
    slot.rep = compact_adt_value_rep_for_type(bundle, type);
    slot.c_type = slot.rep.c_type;
    return slot;
}

static XaotAbiSlot native_value_slot_for_type(const XaotBundle *bundle, const XiFunc *func,
                                              const XrType *type, const XiValue *value,
                                              bool is_return) {
    XaotAbiSlot slot;
    XaotValueRep struct_rep;

    if (type_is_freestanding_ordinal_enum(bundle, type))
        return enum_ordinal_slot(type);
    if (type_is_class_instance_ptr_boundary(bundle, type))
        return ptr_slot(type);
    struct_rep = struct_value_rep_for_slot(bundle, func, type, value, is_return);
    if (struct_rep.kind == XAOT_VALUE_AGGREGATE) {
        memset(&slot, 0, sizeof(slot));
        slot.rep = struct_rep;
        slot.cls = XAOT_ARG_AGG_BY_VALUE;
        slot.c_type = slot.rep.c_type;
        return slot;
    }

    memset(&slot, 0, sizeof(slot));
    slot.rep = xaot_value_rep_for_type(type);
    slot.cls = arg_class_for_value_rep(slot.rep);
    slot.c_type = slot.rep.c_type;
    return slot;
}

static XaotAbiSlot borrowed_place_slot(const XrType *type, XaotAbiSlot value_slot) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    const char *pointee_c_type = value_slot.c_type ? value_slot.c_type : "XrValue";
    size_t n = strlen(pointee_c_type) + 3;
    char *c_type = (char *) xr_malloc(n);
    if (!c_type)
        return tagged_slot(type);
    snprintf(c_type, n, "%s *", pointee_c_type);
    slot.cls = XAOT_ARG_PTR;
    slot.rep.kind = XAOT_VALUE_VIEW;
    slot.rep.rep = XAOT_REP_RAWPTR;
    slot.rep.type = type;
    slot.rep.c_type = c_type;
    slot.rep.flags = XAOT_VALUE_FLAG_OWNED_C_TYPE;
    slot.pointee_rep = value_slot.rep;
    slot.c_type = slot.rep.c_type;
    slot.flags = XAOT_ABI_SLOT_BORROWED_PLACE;
    return slot;
}

static XaotAbiSlot place_value_slot_for_type(const XaotBundle *bundle, const XiFunc *func,
                                             const XrType *type, const XiValue *value) {
    /* Native class pointers and by-value struct aggregates are valid value
     * ABIs, but a ref/out place must match the addressable caller slot.  Their
     * stable call-bound storage is the tagged XrValue slot; scalar, pointer,
     * fixed-array and span places remain native below. */
    if (type && !type->is_nullable) {
        switch (type->kind) {
            case XR_KIND_INT:
            case XR_KIND_FLOAT:
            case XR_KIND_BOOL:
            case XR_KIND_RUNE:
            case XR_KIND_POINTER:
            case XR_KIND_SPAN:
                return native_value_slot_for_type(bundle, func, type, value, false);
            default:
                break;
        }
    }
    return tagged_slot(type);
}

static XaotAbiSlot native_slot_for_type(const XaotBundle *bundle, const XiFunc *func,
                                        const XrType *type, const XiValue *value, bool is_return) {
    if (param_uses_place_abi(func, value, is_return, NULL)) {
        const char *fixed_array_elem_ctype = NULL;
        if (fixed_array_place_param_can_use_ptr_abi(func, value, type, is_return,
                                                    &fixed_array_elem_ctype))
            return fixed_array_place_param_slot(type, fixed_array_elem_ctype);
        return borrowed_place_slot(type, place_value_slot_for_type(bundle, func, type, value));
    }
    return native_value_slot_for_type(bundle, func, type, value, is_return);
}

static bool type_can_use_native_return_boundary(const XaotBundle *bundle, const XiFunc *func,
                                                const XrType *type) {
    if (type && XR_TYPE_IS_UNIT(type))
        return true;
    if (type_is_freestanding_ordinal_enum(bundle, type))
        return true;
    return abi_type_can_use_typed_boundary(bundle, type) ||
           struct_layout_can_use_value_abi_depth(
               struct_layout_for_slot(bundle, func, type, NULL, true), 0) ||
           type_can_use_compact_adt_return(bundle, type);
}

static XaotBoundaryReason tagged_reason_for_func(const XiFunc *func, bool is_module_init) {
    if (is_module_init)
        return XAOT_BOUNDARY_MODULE_INIT;
    if (func && func->ncaptures > 0)
        return XAOT_BOUNDARY_CLOSURE_OBJECT;
    if (func_has_op_class(func, XI_GEN_CLASS_COROUTINE))
        return XAOT_BOUNDARY_CORO_FRAME;
    if (func_has_op_class(func, XI_GEN_CLASS_EXCEPTION))
        return XAOT_BOUNDARY_EXCEPTION_FLOW;
    return XAOT_BOUNDARY_TAGGED_TYPE;
}

XR_FUNC bool xaot_abi_build_func(XaotFuncAbi *abi, const XaotBundle *bundle, const XiFunc *func,
                                 bool is_module_init) {
    bool native_abi;
    uint16_t i;

    if (!abi || !func)
        return false;

    memset(abi, 0, sizeof(*abi));
    /* A vararg function's rest parameter is materialized as a real trailing
     * Array<T> ABI slot. The VM packs varargs at the callee; AOT instead
     * collects the trailing arguments into an array at each direct call site
     * and passes it as this extra parameter. func->nparams excludes the rest
     * slot, so the ABI carries one more parameter than the fixed-arg count. */
    uint16_t abi_nparams = (uint16_t) (func->nparams + (func->is_vararg ? 1 : 0));
    abi->nparams = abi_nparams;
    if (abi_nparams > 0) {
        abi->params = (XaotAbiSlot *) xr_calloc(abi_nparams, sizeof(XaotAbiSlot));
        if (!abi->params)
            return false;
    }

    /* Error/throw flow is carried through xrt_pending_error/xrt_throw_exc and
     * does not require the function parameter/result ABI itself to be tagged. */
    bool native_runtime_callback =
        func->native_callback_kind != XI_NATIVE_CALLBACK_NONE && !is_module_init;

    native_abi = !is_module_init && (native_runtime_callback || func->ncaptures == 0) &&
                 !func_has_op_class(func, XI_GEN_CLASS_COROUTINE) &&
                 type_can_use_native_return_boundary(bundle, func, func->return_type);

    if (!native_abi) {
        abi->kind =
            func_has_op_class(func, XI_GEN_CLASS_COROUTINE) ? XAOT_ABI_CORO : XAOT_ABI_TAGGED;
        abi->boundary_reason = tagged_reason_for_func(func, is_module_init);
        abi->ret = tagged_slot(func->return_type);
        for (i = 0; i < abi_nparams; i++) {
            const XiValue *param = func->params ? func->params[i] : NULL;
            abi->params[i] =
                param_uses_place_abi(func, param, false, NULL)
                    ? native_slot_for_type(bundle, func, param ? param->type : NULL, param, false)
                    : tagged_slot(param ? param->type : NULL);
        }
        return true;
    }

    abi->kind = XAOT_ABI_NATIVE;
    abi->boundary_reason = XAOT_BOUNDARY_NONE;
    if (type_can_use_compact_adt_return(bundle, func->return_type))
        abi->ret = compact_adt_return_slot(bundle, func->return_type);
    else
        abi->ret = native_slot_for_type(bundle, func, func->return_type, NULL, true);
    for (i = 0; i < abi_nparams; i++) {
        const XiValue *param = func->params ? func->params[i] : NULL;
        abi->params[i] =
            native_slot_for_type(bundle, func, param ? param->type : NULL, param, false);
    }
    return true;
}

XR_FUNC void xaot_abi_free(XaotFuncAbi *abi) {
    if (!abi)
        return;
    if ((abi->ret.rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) != 0)
        xr_free((void *) abi->ret.rep.c_type);
    if ((abi->ret.pointee_rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) != 0)
        xr_free((void *) abi->ret.pointee_rep.c_type);
    for (uint16_t i = 0; i < abi->nparams; i++) {
        if ((abi->params[i].rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) != 0)
            xr_free((void *) abi->params[i].rep.c_type);
        if ((abi->params[i].pointee_rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) != 0)
            xr_free((void *) abi->params[i].pointee_rep.c_type);
    }
    xr_free(abi->params);
    memset(abi, 0, sizeof(*abi));
}

XR_FUNC XaotValueRep xaot_abi_slot_value_rep(const XaotAbiSlot *slot) {
    XaotValueRep rep;

    if (!slot) {
        memset(&rep, 0, sizeof(rep));
        rep.kind = XAOT_VALUE_TAGGED;
        rep.rep = XAOT_REP_TAGGED;
        return rep;
    }
    rep = slot->rep;
    if (slot->cls == XAOT_ARG_TAGGED) {
        rep.kind = XAOT_VALUE_TAGGED;
        rep.rep = XAOT_REP_TAGGED;
    }
    return rep;
}

XR_FUNC const char *xaot_abi_kind_name(XaotAbiKind kind) {
    switch (kind) {
        case XAOT_ABI_NATIVE:
            return "native";
        case XAOT_ABI_TAGGED:
            return "tagged";
        case XAOT_ABI_ADAPTER:
            return "adapter";
        case XAOT_ABI_CORO:
            return "coro";
        case XAOT_ABI_RUNTIME_HELPER:
            return "runtime-helper";
        default:
            return "?";
    }
}
