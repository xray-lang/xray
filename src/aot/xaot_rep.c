/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_rep.c - AOT value representation plan
 */

#include "xaot_rep.h"
#include "xaot_abi_gen.h"
#include "../ir/xi.h"
#include "../ir/xi_ops_gen.h"
#include "../runtime/value/xstruct_layout.h"
#include <string.h>

static XaotValueRep value_rep_make(const XrType *type, XaotRep rep) {
    XaotValueRep out;
    const XaotRepInfo *info;

    memset(&out, 0, sizeof(out));
    out.type = type;
    info = xaot_rep_info(rep);
    out.rep = rep;
    out.c_type = info ? info->c_type : "XrValue";

    if (!info || rep == XAOT_REP_TAGGED) {
        out.kind = XAOT_VALUE_TAGGED;
        out.c_type = "XrValue";
        return out;
    }
    if (rep == XAOT_REP_VOID) {
        out.kind = XAOT_VALUE_VOID;
        return out;
    }
    if (rep == XAOT_REP_PTR) {
        out.kind = XAOT_VALUE_PTR;
        return out;
    }
    out.kind = XAOT_VALUE_SCALAR;
    return out;
}

XR_FUNC XaotValueRep xaot_value_rep_for_type(const XrType *type) {
    return value_rep_make(type, xaot_abi_rep_for_type(type));
}

static bool rep_from_native_name(const char *name, XaotRep *out) {
    if (!name)
        return false;
    if (strcmp(name, "i8") == 0) {
        if (out)
            *out = XAOT_REP_I8;
        return true;
    }
    if (strcmp(name, "u8") == 0) {
        if (out)
            *out = XAOT_REP_U8;
        return true;
    }
    if (strcmp(name, "i16") == 0) {
        if (out)
            *out = XAOT_REP_I16;
        return true;
    }
    if (strcmp(name, "u16") == 0) {
        if (out)
            *out = XAOT_REP_U16;
        return true;
    }
    if (strcmp(name, "i32") == 0) {
        if (out)
            *out = XAOT_REP_I32;
        return true;
    }
    if (strcmp(name, "u32") == 0) {
        if (out)
            *out = XAOT_REP_U32;
        return true;
    }
    if (strcmp(name, "f32") == 0) {
        if (out)
            *out = XAOT_REP_F32;
        return true;
    }
    return false;
}

static bool rep_from_xr_storage(const XrType *type, XrRep storage, XaotRep *out) {
    XaotRep type_rep;

    if (!out)
        return false;
    switch (storage) {
        case XR_REP_I64:
            type_rep = xaot_abi_rep_for_type(type);
            if (type_rep != XAOT_REP_TAGGED &&
                xaot_value_storage_rep(value_rep_make(type, type_rep)) == XR_REP_I64) {
                *out = type_rep;
                return true;
            }
            *out = XAOT_REP_I64;
            return true;
        case XR_REP_F64:
            type_rep = xaot_abi_rep_for_type(type);
            if (type_rep != XAOT_REP_TAGGED &&
                xaot_value_storage_rep(value_rep_make(type, type_rep)) == XR_REP_F64) {
                *out = type_rep;
                return true;
            }
            *out = XAOT_REP_F64;
            return true;
        case XR_REP_PTR:
            *out = XAOT_REP_PTR;
            return true;
        case XR_REP_VOID:
            *out = XAOT_REP_VOID;
            return true;
        case XR_REP_TAGGED:
        default:
            *out = XAOT_REP_TAGGED;
            return true;
    }
}

static const XiValue *trace_fixed_array_field_ref(const XiValue *v) {
    while (v && (xi_copy_is_identity_alias(v) || v->op == XI_MOVE) && v->nargs >= 1)
        v = v->args[0];
    if (!v || v->op != XI_STRUCT_GET || v->nargs < 1)
        return NULL;
    const XrStructLayout *sl = (const XrStructLayout *) v->aux;
    if (!sl || v->aux_int < 0 || v->aux_int >= sl->field_count)
        return NULL;
    const XrStructFieldLayout *field = &sl->fields[v->aux_int];
    return field->native_type == XR_NATIVE_ARRAY ? v : NULL;
}

static bool fixed_array_elem_rep_for_value(const XiValue *value, XaotRep *out) {
    const XiValue *ref;
    const XrStructLayout *sl;
    const XrStructFieldLayout *field;

    if (!value)
        return false;
    if (value->op != XI_INDEX_GET && value->op != XI_INDEX_SET)
        return false;
    if (value->nargs < 1)
        return false;
    ref = trace_fixed_array_field_ref(value->args[0]);
    if (!ref)
        return false;
    sl = (const XrStructLayout *) ref->aux;
    field = &sl->fields[ref->aux_int];
    return xaot_rep_from_native_type(field->elem_native_type, out);
}

static XaotValueRep fixed_array_view_rep(const XiValue *value) {
    XaotValueRep rep = value_rep_make(value ? value->type : NULL, XAOT_REP_PTR);
    rep.kind = XAOT_VALUE_VIEW;
    return rep;
}

XR_FUNC XaotValueRep xaot_value_rep_for_value(const XiValue *value) {
    XaotRep rep;

    if (!value)
        return xaot_value_rep_for_type(NULL);
    if ((value->type && XR_TYPE_IS_UNIT(value->type)) ||
        xi_generated_op_result_kind(value->op) == XI_GEN_RESULT_VOID)
        return value_rep_make(value->type, XAOT_REP_VOID);
    if (trace_fixed_array_field_ref(value))
        return fixed_array_view_rep(value);
    if (fixed_array_elem_rep_for_value(value, &rep))
        return value_rep_make(value->type, rep);
    if (rep_from_native_name(xi_generated_op_result_native_type(value->op), &rep))
        return value_rep_make(value->type, rep);
    if (value->op == XI_WIDEN_F32)
        return value_rep_make(value->type, XAOT_REP_F64);
    if (value->op == XI_WIDEN_I8 || value->op == XI_WIDEN_U8 || value->op == XI_WIDEN_I16 ||
        value->op == XI_WIDEN_U16 || value->op == XI_WIDEN_I32 || value->op == XI_WIDEN_U32)
        return value_rep_make(value->type, XAOT_REP_I64);
    if (rep_from_xr_storage(value->type, (XrRep) value->rep, &rep))
        return value_rep_make(value->type, rep);
    return xaot_value_rep_for_type(value->type);
}

XR_FUNC XrRep xaot_value_storage_rep(XaotValueRep rep) {
    const XaotRepInfo *info = xaot_rep_info(rep.rep);
    return info ? info->storage_rep : XR_REP_TAGGED;
}

XR_FUNC const char *xaot_value_kind_name(XaotValueKind kind) {
    switch (kind) {
        case XAOT_VALUE_VOID:
            return "void";
        case XAOT_VALUE_SCALAR:
            return "scalar";
        case XAOT_VALUE_TAGGED:
            return "tagged";
        case XAOT_VALUE_PTR:
            return "ptr";
        case XAOT_VALUE_AGGREGATE:
            return "aggregate";
        case XAOT_VALUE_VIEW:
            return "view";
        default:
            return "?";
    }
}
