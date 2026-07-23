/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_container.c - AOT typed container provenance plan
 */

#include "xaot_container.h"
#include <string.h>

static uint64_t type_key_hash_byte(uint64_t hash, uint8_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t type_key_fingerprint(const XaotTypeKey *key) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!key)
        return 0;
    hash = type_key_hash_byte(hash, (uint8_t) key->kind);
    hash = type_key_hash_byte(hash, (uint8_t) key->container_kind);
    hash = type_key_hash_byte(hash, (uint8_t) key->elem_rep);
    hash = type_key_hash_byte(hash, (uint8_t) key->key_rep);
    hash = type_key_hash_byte(hash, (uint8_t) key->value_rep);
    hash = type_key_hash_byte(hash, (uint8_t) key->elem_storage_rep);
    hash = type_key_hash_byte(hash, (uint8_t) key->key_storage_rep);
    hash = type_key_hash_byte(hash, (uint8_t) key->value_storage_rep);
    return hash;
}

static bool container_plan_finalize_key(XaotContainerPlan *plan) {
    if (!plan)
        return false;
    memset(&plan->type_key, 0, sizeof(plan->type_key));
    plan->type_key.kind = XAOT_TYPE_KEY_CONTAINER;
    plan->type_key.container_kind = plan->kind;
    if (plan->kind == XAOT_CONTAINER_MAP) {
        plan->type_key.key_rep = plan->key.rep;
        plan->type_key.value_rep = plan->value.rep;
        plan->type_key.key_storage_rep = plan->key.storage_rep;
        plan->type_key.value_storage_rep = plan->value.storage_rep;
    } else {
        plan->type_key.elem_rep = plan->elem.rep;
        plan->type_key.elem_storage_rep = plan->elem.storage_rep;
    }
    plan->type_key.fingerprint = type_key_fingerprint(&plan->type_key);
    return plan->type_key.fingerprint != 0;
}

static bool elem_plan_make(const XrType *type, XaotRep rep, const char *elem_name,
                           XaotContainerElemPlan *out) {
    const XaotRepInfo *info;

    if (!type || !elem_name || !out)
        return false;
    info = xaot_rep_info(rep);
    if (!info || !info->c_type)
        return false;

    memset(out, 0, sizeof(*out));
    out->type = type;
    out->rep = rep;
    out->storage_rep = info->storage_rep;
    out->elem_name = elem_name;
    out->c_type = info->c_type;
    return true;
}

static bool container_elem_plan_for_type(const XrType *type, XaotContainerElemPlan *out,
                                         bool allow_char) {
    XaotRep rep;
    const char *elem_name;

    if (!type || type->is_nullable || !out)
        return false;

    if (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT) {
        elem_name = xaot_elem_name_for_native_type(type->scalar_rep);
        if (elem_name && xaot_rep_from_native_type(type->scalar_rep, &rep))
            return elem_plan_make(type, rep, elem_name, out);
    }

    if (type->kind == XR_KIND_INT)
        return elem_plan_make(type, XAOT_REP_I64, "XR_ELEM_I64", out);
    if (type->kind == XR_KIND_FLOAT)
        return elem_plan_make(type, XAOT_REP_F64, "XR_ELEM_F64", out);
    if (type->kind == XR_KIND_BOOL)
        return elem_plan_make(type, XAOT_REP_BOOL, "XR_ELEM_BOOL", out);
    if (allow_char && type->kind == XR_KIND_RUNE)
        return elem_plan_make(type, XAOT_REP_RUNE, "XR_ELEM_RUNE", out);
    /* CFn<...> is a bare C function pointer: store the raw address (8 bytes,
     * GC-invisible) instead of a tagged closure. */
    if (XR_TYPE_IS_C_FUNCTION(type))
        return elem_plan_make(type, XAOT_REP_RAWPTR, "XR_ELEM_RAWPTR", out);
    return false;
}

static bool array_elem_plan_for_type(const XrType *type, XaotContainerElemPlan *out) {
    if (!type || !out)
        return false;
    if (container_elem_plan_for_type(type, out, true))
        return true;
    return elem_plan_make(type, XAOT_REP_TAGGED, "XR_ELEM_ANY", out);
}

XR_FUNC bool xaot_container_elem_plan_for_type(const XrType *type, XaotContainerElemPlan *out) {
    return container_elem_plan_for_type(type, out, false);
}

static const XrType *array_elem_type_from_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_SLICE)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static bool elem_plan_uses_direct_map_helper(const XaotContainerElemPlan *elem) {
    return elem && (elem->storage_rep == XR_REP_I64 || elem->storage_rep == XR_REP_F64);
}

static bool type_contains_unresolved_type_param_depth(const XrType *type, uint8_t depth) {
    if (!type || depth > 16)
        return false;
    if (type->kind == XR_KIND_TYPE_PARAM)
        return true;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
            return type_contains_unresolved_type_param_depth(type->container.element_type,
                                                             depth + 1);
        case XR_KIND_MAP:
            return type_contains_unresolved_type_param_depth(type->map.key_type, depth + 1) ||
                   type_contains_unresolved_type_param_depth(type->map.value_type, depth + 1);
        case XR_KIND_FUNCTION:
            for (int i = 0; i < type->function.param_count; i++) {
                if (type_contains_unresolved_type_param_depth(xr_type_function_param_type(type, i),
                                                              depth + 1))
                    return true;
            }
            return type_contains_unresolved_type_param_depth(type->function.return_type, depth + 1);
        case XR_KIND_INSTANCE:
            for (int i = 0; i < type->instance.type_arg_count; i++) {
                if (type_contains_unresolved_type_param_depth(
                        type->instance.type_args ? type->instance.type_args[i] : NULL, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                if (type_contains_unresolved_type_param_depth(
                        type->tuple.element_types ? type->tuple.element_types[i] : NULL, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_UNION:
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (type_contains_unresolved_type_param_depth(
                        type->union_type.members ? type->union_type.members[i] : NULL, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_FIXED_ARRAY:
            return type_contains_unresolved_type_param_depth(type->fixed_array.element_type,
                                                             depth + 1);
        default:
            return false;
    }
}

XR_FUNC bool xaot_type_contains_unresolved_type_param(const XrType *type) {
    return type_contains_unresolved_type_param_depth(type, 0);
}

XR_FUNC bool xaot_container_plan_for_type(const XrType *type, XaotContainerPlan *out) {
    const XrType *elem;

    if (!type || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = type;

    elem = array_elem_type_from_type(type);
    if (elem) {
        if (!array_elem_plan_for_type(elem, &out->elem))
            return false;
        out->kind = XAOT_CONTAINER_ARRAY;
        out->flags = XAOT_CONTAINER_TYPED_STORAGE | XAOT_CONTAINER_RAW_DATA;
        return container_plan_finalize_key(out);
    }

    if (type->kind == XR_KIND_SET) {
        if (!xaot_container_elem_plan_for_type(type->container.element_type, &out->elem))
            return false;
        out->kind = XAOT_CONTAINER_SET;
        out->flags = XAOT_CONTAINER_TYPED_STORAGE | XAOT_CONTAINER_DIRECT_HELPERS;
        return container_plan_finalize_key(out);
    }

    if (type->kind == XR_KIND_MAP) {
        if (!xaot_container_elem_plan_for_type(type->map.key_type, &out->key) ||
            !xaot_container_elem_plan_for_type(type->map.value_type, &out->value))
            return false;
        out->kind = XAOT_CONTAINER_MAP;
        out->flags = XAOT_CONTAINER_TYPED_STORAGE;
        if (elem_plan_uses_direct_map_helper(&out->key) &&
            elem_plan_uses_direct_map_helper(&out->value))
            out->flags |= XAOT_CONTAINER_DIRECT_HELPERS;
        return container_plan_finalize_key(out);
    }

    return false;
}

XR_FUNC bool xaot_type_key_equal(const XaotTypeKey *a, const XaotTypeKey *b) {
    if (!a || !b)
        return false;
    return a->kind == b->kind && a->container_kind == b->container_kind &&
           a->elem_rep == b->elem_rep && a->key_rep == b->key_rep && a->value_rep == b->value_rep &&
           a->elem_storage_rep == b->elem_storage_rep && a->key_storage_rep == b->key_storage_rep &&
           a->value_storage_rep == b->value_storage_rep && a->fingerprint == b->fingerprint;
}

static bool elem_plan_equal(const XaotContainerElemPlan *a, const XaotContainerElemPlan *b) {
    if (!a || !b || !a->elem_name || !b->elem_name || !a->c_type || !b->c_type)
        return false;
    return a->rep == b->rep && a->storage_rep == b->storage_rep &&
           strcmp(a->elem_name, b->elem_name) == 0 && strcmp(a->c_type, b->c_type) == 0;
}

XR_FUNC bool xaot_container_plan_matches_type(const XaotContainerPlan *plan, const XrType *type) {
    XaotContainerPlan scratch;

    if (!plan || !type)
        return false;
    if (!xaot_container_plan_for_type(type, &scratch))
        return false;
    if (!xaot_type_key_equal(&plan->type_key, &scratch.type_key))
        return false;
    if ((plan->flags & (XAOT_CONTAINER_TYPED_STORAGE | XAOT_CONTAINER_DIRECT_HELPERS |
                        XAOT_CONTAINER_RAW_DATA)) !=
        (scratch.flags &
         (XAOT_CONTAINER_TYPED_STORAGE | XAOT_CONTAINER_DIRECT_HELPERS | XAOT_CONTAINER_RAW_DATA)))
        return false;
    if (plan->kind == XAOT_CONTAINER_MAP)
        return elem_plan_equal(&plan->key, &scratch.key) &&
               elem_plan_equal(&plan->value, &scratch.value);
    return elem_plan_equal(&plan->elem, &scratch.elem);
}

XR_FUNC const char *xaot_container_kind_name(XaotContainerKind kind) {
    switch (kind) {
        case XAOT_CONTAINER_ARRAY:
            return "array";
        case XAOT_CONTAINER_MAP:
            return "map";
        case XAOT_CONTAINER_SET:
            return "set";
        default:
            return "?";
    }
}
