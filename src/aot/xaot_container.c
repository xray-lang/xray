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

XR_FUNC bool xaot_container_elem_plan_for_type(const XrType *type, XaotContainerElemPlan *out) {
    XaotRep rep;
    const char *elem_name;

    if (!type || type->is_nullable || !out)
        return false;

    if (type->native_width != 0) {
        elem_name = xaot_elem_name_for_native_type(type->native_width);
        if (elem_name && xaot_rep_from_native_type(type->native_width, &rep))
            return elem_plan_make(type, rep, elem_name, out);
    }

    if (type->kind == XR_KIND_INT)
        return elem_plan_make(type, XAOT_REP_I64, "XR_ELEM_I64", out);
    if (type->kind == XR_KIND_FLOAT)
        return elem_plan_make(type, XAOT_REP_F64, "XR_ELEM_F64", out);
    if (type->kind == XR_KIND_BOOL)
        return elem_plan_make(type, XAOT_REP_BOOL, "XR_ELEM_BOOL", out);
    return false;
}

static const XrType *array_elem_type_from_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static bool elem_plan_uses_direct_map_helper(const XaotContainerElemPlan *elem) {
    return elem && (elem->storage_rep == XR_REP_I64 || elem->storage_rep == XR_REP_F64);
}

XR_FUNC bool xaot_container_plan_for_type(const XrType *type, XaotContainerPlan *out) {
    const XrType *elem;

    if (!type || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = type;

    elem = array_elem_type_from_type(type);
    if (elem) {
        if (!xaot_container_elem_plan_for_type(elem, &out->elem))
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
