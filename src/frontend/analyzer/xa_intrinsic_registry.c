/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 */

#include "xa_intrinsic_registry.h"
#include "../../runtime/value/xtype.h"

#include <stdio.h>
#include <string.h>

#define XA_NATIVE_NONE 0
#define XA_NATIVE_U8 XR_NATIVE_U8
#define XA_NATIVE_U32 XR_NATIVE_U32
#define XA_NATIVE_U64 XR_NATIVE_U64

static const XaIntrinsicDesc g_intrinsics[] = {
#define XA_INTRINSIC(id, numeric_id, key, family, lowering, effect, allocation, safety, min_arity, \
                     max_arity, flags, input_native, input_lanes, result_native, result_lanes)     \
    {XA_INTRINSIC_##id,                                                                            \
     key,                                                                                          \
     XA_INTRINSIC_FAMILY_##family,                                                                 \
     XA_INTRINSIC_LOWERING_##lowering,                                                             \
     {XA_NATIVE_##input_native, input_lanes, XA_NATIVE_##result_native, result_lanes},             \
     XA_INTRINSIC_EFFECT_##effect,                                                                 \
     XA_INTRINSIC_ALLOCATION_##allocation,                                                         \
     XA_INTRINSIC_SAFETY_##safety,                                                                 \
     min_arity,                                                                                    \
     max_arity,                                                                                    \
     XA_INTRINSIC_FLAG_##flags},
#include "xa_intrinsic_registry.def"
#undef XA_INTRINSIC
};

typedef struct XaSemanticTypeDesc {
    XaSemanticTypeId id;
    const char *key;
    const char *source_name;
} XaSemanticTypeDesc;

static const XaSemanticTypeDesc g_semantic_types[] = {
    {XA_SEMANTIC_TYPE_PARALLEL_PLAN, "parallel.Plan", "Plan"},
    {XA_SEMANTIC_TYPE_PARALLEL_OPTIONS, "parallel.Options", "Options"},
};

XaSemanticTypeId xa_semantic_type_by_key(const char *key) {
    if (!key)
        return XA_SEMANTIC_TYPE_NONE;
    for (size_t i = 0; i < sizeof(g_semantic_types) / sizeof(g_semantic_types[0]); i++) {
        if (strcmp(g_semantic_types[i].key, key) == 0)
            return g_semantic_types[i].id;
    }
    return XA_SEMANTIC_TYPE_NONE;
}

const char *xa_semantic_type_source_name(XaSemanticTypeId id) {
    for (size_t i = 0; i < sizeof(g_semantic_types) / sizeof(g_semantic_types[0]); i++) {
        if (g_semantic_types[i].id == id)
            return g_semantic_types[i].source_name;
    }
    return NULL;
}

const XaIntrinsicDesc *xa_intrinsic_by_id(XaIntrinsicId id) {
    for (size_t i = 0; i < sizeof(g_intrinsics) / sizeof(g_intrinsics[0]); i++) {
        if (g_intrinsics[i].id == id)
            return &g_intrinsics[i];
    }
    return NULL;
}

const XaIntrinsicDesc *xa_intrinsic_by_key(const char *key) {
    if (!key)
        return NULL;
    for (size_t i = 0; i < sizeof(g_intrinsics) / sizeof(g_intrinsics[0]); i++) {
        if (strcmp(g_intrinsics[i].key, key) == 0)
            return &g_intrinsics[i];
    }
    return NULL;
}

const char *xa_intrinsic_source_member(const XaIntrinsicDesc *desc) {
    if (!desc || !desc->key)
        return NULL;
    const char *dot = strrchr(desc->key, '.');
    return dot ? dot + 1 : desc->key;
}

XaIntrinsicId xa_intrinsic_compiler_receiver_method(const XrType *receiver,
                                                    const char *member_name) {
    if (!receiver || !member_name || receiver->kind != XR_KIND_INSTANCE ||
        !xr_type_is_named_class(receiver, "Atomic"))
        return XA_INTRINSIC_NONE;
    for (size_t i = 0; i < xa_intrinsic_count(); i++) {
        const XaIntrinsicDesc *desc = &g_intrinsics[i];
        if (desc->family == XA_INTRINSIC_FAMILY_ATOMIC &&
            strcmp(xa_intrinsic_source_member(desc), member_name) == 0)
            return desc->id;
    }
    return XA_INTRINSIC_NONE;
}

size_t xa_intrinsic_count(void) {
    return sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
}

const XaIntrinsicDesc *xa_intrinsic_at(size_t index) {
    return index < xa_intrinsic_count() ? &g_intrinsics[index] : NULL;
}

bool xa_intrinsic_registry_validate(char *error, size_t error_size) {
    for (size_t i = 0; i < xa_intrinsic_count(); i++) {
        const XaIntrinsicDesc *a = &g_intrinsics[i];
        const char *member = xa_intrinsic_source_member(a);
        if (a->id == XA_INTRINSIC_NONE || !a->key || !a->key[0] || !member || !member[0] ||
            a->min_arity > a->max_arity || a->lowering == XA_INTRINSIC_LOWERING_NONE) {
            if (error && error_size)
                snprintf(error, error_size, "invalid intrinsic descriptor at index %zu", i);
            return false;
        }
        for (size_t j = i + 1; j < xa_intrinsic_count(); j++) {
            const XaIntrinsicDesc *b = &g_intrinsics[j];
            if (a->id == b->id || strcmp(a->key, b->key) == 0) {
                if (error && error_size)
                    snprintf(error, error_size, "duplicate intrinsic %s", a->key);
                return false;
            }
        }
    }
    if (error && error_size)
        error[0] = '\0';
    return true;
}
