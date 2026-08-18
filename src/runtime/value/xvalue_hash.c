/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_hash.c - XrValue-aware hash functions implementation
 */

#include "xvalue_hash.h"
#include "../class/xenum.h"
#include "../../base/xchecks.h"
#include "../../shared/xr_hash_core.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../object/xstring.h"
#include <math.h>

#define XR_DERIVED_HASH_MAX_DEPTH 64

/* Installed by the VM (xr_value_set_instance_hooks). NULL in backends that do
 * not run user methods, where instance keys fall back to pointer identity. */
static XrValueInstanceHashHook g_instance_hash_hook;
static XrValueInstanceEqHook g_instance_eq_hook;

void xr_value_set_instance_hooks(XrValueInstanceHashHook hash_hook, XrValueInstanceEqHook eq_hook) {
    g_instance_hash_hook = hash_hook;
    g_instance_eq_hook = eq_hook;
}

uint32_t xr_hash_string(XrString *str) {
    XR_DCHECK(str != NULL, "hash_string: NULL string");
    if (str->hash == 0) {
        uint32_t h = xr_string_hash(str->data, str->length);
        h = (h == 0) ? 1 : h;
        if (str->header.domain_id == XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL)
            str->hash = h;
        return h;
    }
    return str->hash;
}

static uint32_t xr_hash_value_depth(XrValue val, uint32_t depth) {
    switch (val.tag) {
        case XR_TAG_NULL:
            return XR_HASH_NULL;
        case XR_TAG_BOOL:
            return xr_hash_bool((int) val.i);
        case XR_TAG_RUNE:
            return xr_hash_int(val.i);
        case XR_TAG_I64:
            return xr_hash_int(val.i);
        case XR_TAG_F64:
            return xr_hash_float(val.f);
        case XR_TAG_PTR:
            if (val.heap_type == XR_TSTRING) {
                return xr_hash_string(XR_TO_STRING(val));
            }
            if (val.heap_type == XR_TINSTANCE && val.ptr) {
                XrInstance *instance = (XrInstance *) val.ptr;
                XrClass *cls = instance->klass;
                /* ADT enum aggregates key by content -- nominal identity, member
                 * index, then payloads -- so content-equal enum values land in
                 * the same bucket regardless of which allocation carries them.
                 * Must stay consistent with the aggregate branch in
                 * value_eq_core below. */
                if (cls && cls->builtin_kind == XR_BK_ADT_ENUM) {
                    const XrEnumAggregateValue *agg = (const XrEnumAggregateValue *) val.ptr;
                    const XrEnumType *et = xr_enum_aggregate_type(agg);
                    uint32_t layout_id = (et && et->layout) ? et->layout->layout_id : 0;
                    uint64_t nominal = layout_id
                                           ? (((uint64_t) layout_id << 32) | agg->member_index)
                                           : (((uint64_t) (uintptr_t) et) ^ agg->member_index);
                    uint64_t hash = xr_hash_core_mix_u64(nominal);
                    if (depth < XR_DERIVED_HASH_MAX_DEPTH) {
                        for (uint32_t i = 0; i < agg->payload_count; i++) {
                            uint32_t payload_hash =
                                xr_hash_value_depth(agg->payloads[i], depth + 1u);
                            hash ^= (uint64_t) payload_hash + UINT64_C(0x9e3779b97f4a7c15) +
                                    (hash << 6) + (hash >> 2);
                        }
                    }
                    uint32_t folded = (uint32_t) (hash ^ (hash >> 32));
                    return folded ? folded : 1u;
                }
                /* A hand-written hash() keys by value. It is invoked through the
                 * hook (only the VM can run it) and takes precedence over the
                 * pointer fallback; @derive(Hash) classes declare no such method
                 * and stay on the structural path below. */
                if (g_instance_hash_hook) {
                    uint32_t user_hash = 0;
                    if (g_instance_hash_hook(val, &user_hash))
                        return user_hash ? user_hash : 1u;
                }
                if (cls && (cls->flags & XR_CLASS_DERIVE_HASH) != 0) {
                    uint64_t hash = xr_hash_core_mix_u64((uint64_t) (uintptr_t) cls);
                    if (depth >= XR_DERIVED_HASH_MAX_DEPTH) {
                        uint32_t capped = (uint32_t) hash;
                        return capped ? capped : 1u;
                    }
                    uint32_t field_count = xr_class_instance_field_count(cls);
                    for (uint32_t i = 0; i < field_count; i++) {
                        uint32_t field_hash = xr_hash_value_depth(instance->fields[i], depth + 1u);
                        hash ^= (uint64_t) field_hash + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) +
                                (hash >> 2);
                    }
                    uint32_t folded = (uint32_t) (hash ^ (hash >> 32));
                    return folded ? folded : 1u;
                }
            }
            {
                uintptr_t ptr = (uintptr_t) val.ptr;
                return (uint32_t) (ptr ^ (ptr >> 16));
            }
        default:
            return 0;
    }
}

uint32_t xr_hash_value(XrValue val) {
    return xr_hash_value_depth(val, 0);
}

/* One comparison body serves two relations that differ only on NaN: `==` is
 * IEEE, and the container key relation is reflexive. Splitting them into
 * separate implementations would let the shared cases drift apart. */
static bool value_eq_core(XrValue a, XrValue b, bool key_equivalence) {
    // Fast path: same tag and value
    if (xr_value_same(a, b))
        return true;

    XrTypeId tid_a = xr_value_typeid(a);
    XrTypeId tid_b = xr_value_typeid(b);

    if (tid_a != tid_b)
        return false;

    if (tid_a == XR_TID_NULL)
        return true;
    if (tid_a == XR_TID_BOOL)
        return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (tid_a == XR_TID_RUNE)
        return XR_TO_RUNE(a) == XR_TO_RUNE(b);
    if (XR_TID_IS_INT(tid_a))
        return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_TID_IS_FLOAT(tid_a)) {
        double fa = XR_TO_FLOAT(a);
        double fb = XR_TO_FLOAT(b);
        if (key_equivalence)
            return xr_hash_core_key_eq_f64(fa, fb) != 0;
        if (isnan(fa) || isnan(fb))
            return false;
        if (fa == 0.0 && fb == 0.0)
            return true;
        return fa == fb;
    }
    if (tid_a == XR_TID_STRING) {
        XrString *s1 = XR_TO_STRING(a);
        XrString *s2 = XR_TO_STRING(b);
        if (s1 == s2)
            return true;
        if (s1->length != s2->length)
            return false;
        return memcmp(s1->data, s2->data, s1->length) == 0;
    }
    // Heap objects: pointer (reference) equality
    if (XR_IS_PTR(a) && XR_IS_PTR(b)) {
        if (a.heap_type == XR_TINSTANCE && b.heap_type == XR_TINSTANCE && a.ptr && b.ptr) {
            XrInstance *ia = (XrInstance *) a.ptr;
            XrInstance *ib = (XrInstance *) b.ptr;
            /* ADT enum aggregates compare by content: nominal identity, member
             * index, then payloads. Mirrors the hash branch above so a payload
             * enum behaves the same as a container key and under `==`. */
            if (ia->klass && ia->klass->builtin_kind == XR_BK_ADT_ENUM && ib->klass &&
                ib->klass->builtin_kind == XR_BK_ADT_ENUM) {
                const XrEnumAggregateValue *ea = (const XrEnumAggregateValue *) a.ptr;
                const XrEnumAggregateValue *eb = (const XrEnumAggregateValue *) b.ptr;
                if (!xr_enum_type_same_nominal(xr_enum_aggregate_type(ea),
                                               xr_enum_aggregate_type(eb)) ||
                    ea->member_index != eb->member_index || ea->payload_count != eb->payload_count)
                    return false;
                for (uint32_t i = 0; i < ea->payload_count; i++) {
                    if (!value_eq_core(ea->payloads[i], eb->payloads[i], key_equivalence))
                        return false;
                }
                return true;
            }
            /* A hand-written operator == decides equality by value; the hook
             * runs it (VM only) and takes precedence over the address compare,
             * mirroring the hash hook so a user Hashable key round-trips. */
            if (g_instance_eq_hook) {
                int handled = g_instance_eq_hook(a, b);
                if (handled >= 0)
                    return handled != 0;
            }
            if (ia->klass && ia->klass == ib->klass &&
                (ia->klass->flags & XR_CLASS_DERIVE_EQ) != 0) {
                return key_equivalence ? xr_value_deep_key_eq(a, b) : xr_value_deep_eq(a, b);
            }
        }
        return XR_TO_PTR(a) == XR_TO_PTR(b);
    }
    return false;
}

bool xr_value_eq(XrValue a, XrValue b) {
    return value_eq_core(a, b, false);
}

bool xr_value_key_eq(XrValue a, XrValue b) {
    return value_eq_core(a, b, true);
}
