/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_deep_equality.h - Target-neutral deep equality traversal
 *
 * This traversal is the single structural-decision owner for VM and AOT.
 * Target adapters expose storage and the language Map/Set key-equivalence
 * authority.  They do not choose which aggregates recurse, which identities
 * are nominal, or how value equality classifies cycles.  Map/Set membership is
 * deliberately key equivalence (NaN is reflexive and signed zero is folded),
 * not value equality; every backend adapter must delegate to its canonical
 * collection lookup rather than reimplement that relation here.
 *
 * Cycle equality is coinductive: revisiting an active (left,right) pair proves
 * that edge.  This is the pre-existing language contract and intentionally
 * compares bisimilar graphs equal even when their cycle node counts differ.
 */

#ifndef XR_DEEP_EQUALITY_H
#define XR_DEEP_EQUALITY_H

#include "xray_value_abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { XR_DEEP_EQUALITY_MAX_DEPTH = 256 };

typedef enum XrDeepEqualityKind {
    XR_DEEP_EQUALITY_INVALID = 0,
    XR_DEEP_EQUALITY_IDENTITY,
    XR_DEEP_EQUALITY_STRING,
    XR_DEEP_EQUALITY_ARRAY,
    XR_DEEP_EQUALITY_TUPLE,
    XR_DEEP_EQUALITY_MAP,
    XR_DEEP_EQUALITY_SET,
    XR_DEEP_EQUALITY_STRUCT_OBJECT,
    XR_DEEP_EQUALITY_DERIVED_INSTANCE,
    XR_DEEP_EQUALITY_ENUM,
    XR_DEEP_EQUALITY_KIND_COUNT,
} XrDeepEqualityKind;

typedef struct XrDeepEqualityNode {
    XrDeepEqualityKind kind;
    const void *identity;
    uint64_t nominal_identity;
    uint32_t logical_count;
    uint32_t iteration_extent;
    uint32_t ordinal;
} XrDeepEqualityNode;

typedef struct XrDeepEqualityOps {
    bool (*describe)(void *adapter, XrValue value, XrDeepEqualityNode *out);
    bool (*fallback_equal)(void *adapter, XrValue left, XrValue right);
    bool (*string_equal)(void *adapter, XrValue left, XrValue right);
    bool (*sequence_element)(void *adapter, XrValue sequence, uint32_t index, XrValue *out);
    bool (*map_entry)(void *adapter, XrValue map, uint32_t slot, bool *present, XrValue *key,
                      XrValue *value);
    bool (*map_find_key_equivalent)(void *adapter, XrValue map, XrValue key, bool *found,
                                    XrValue *value);
    bool (*set_entry)(void *adapter, XrValue set, uint32_t slot, bool *present, XrValue *value);
    bool (*set_contains_key_equivalent)(void *adapter, XrValue set, XrValue value,
                                        bool *contains);
    bool (*struct_field_pair)(void *adapter, XrValue left, XrValue right, uint32_t left_ordinal,
                              XrValue *left_value, XrValue *right_value);
} XrDeepEqualityOps;

typedef struct XrDeepEqualityPair {
    const void *left;
    const void *right;
} XrDeepEqualityPair;

typedef struct XrDeepEqualityContext {
    const XrDeepEqualityOps *ops;
    void *adapter;
    uint16_t pair_count;
    XrDeepEqualityPair pairs[XR_DEEP_EQUALITY_MAX_DEPTH];
} XrDeepEqualityContext;

static inline bool xr_deep_equality_value(XrDeepEqualityContext *ctx, XrValue left,
                                          XrValue right);

static inline bool xr_deep_equality_enter(XrDeepEqualityContext *ctx,
                                          const XrDeepEqualityNode *left,
                                          const XrDeepEqualityNode *right,
                                          bool *already_seen) {
    if (!ctx || !left || !right || !left->identity || !right->identity || !already_seen)
        return false;
    for (uint16_t i = 0; i < ctx->pair_count; i++) {
        if (ctx->pairs[i].left == left->identity && ctx->pairs[i].right == right->identity) {
            *already_seen = true;
            return true;
        }
    }
    if (ctx->pair_count >= XR_DEEP_EQUALITY_MAX_DEPTH)
        return false;
    *already_seen = false;
    ctx->pairs[ctx->pair_count++] =
        (XrDeepEqualityPair){left->identity, right->identity};
    return true;
}

static inline void xr_deep_equality_leave(XrDeepEqualityContext *ctx, bool already_seen) {
    if (ctx && !already_seen && ctx->pair_count != 0)
        ctx->pair_count--;
}

static inline bool xr_deep_equality_sequence(XrDeepEqualityContext *ctx, XrValue left,
                                             XrValue right, const XrDeepEqualityNode *left_node,
                                             const XrDeepEqualityNode *right_node) {
    if (!ctx->ops->sequence_element || left_node->logical_count != right_node->logical_count)
        return false;
    bool already_seen = false;
    if (!xr_deep_equality_enter(ctx, left_node, right_node, &already_seen))
        return false;
    bool equal = true;
    for (uint32_t i = 0; !already_seen && equal && i < left_node->logical_count; i++) {
        XrValue left_value = {0};
        XrValue right_value = {0};
        equal = ctx->ops->sequence_element(ctx->adapter, left, i, &left_value) &&
                ctx->ops->sequence_element(ctx->adapter, right, i, &right_value) &&
                xr_deep_equality_value(ctx, left_value, right_value);
    }
    xr_deep_equality_leave(ctx, already_seen);
    return equal;
}

static inline bool xr_deep_equality_map(XrDeepEqualityContext *ctx, XrValue left, XrValue right,
                                        const XrDeepEqualityNode *left_node,
                                        const XrDeepEqualityNode *right_node) {
    if (!ctx->ops->map_entry || !ctx->ops->map_find_key_equivalent ||
        left_node->logical_count != right_node->logical_count)
        return false;
    bool already_seen = false;
    if (!xr_deep_equality_enter(ctx, left_node, right_node, &already_seen))
        return false;
    bool equal = true;
    for (uint32_t slot = 0; !already_seen && equal && slot < left_node->iteration_extent; slot++) {
        bool present = false;
        XrValue key = {0};
        XrValue left_value = {0};
        XrValue right_value = {0};
        bool found = false;
        equal = ctx->ops->map_entry(ctx->adapter, left, slot, &present, &key, &left_value);
        if (!equal || !present)
            continue;
        equal = ctx->ops->map_find_key_equivalent(ctx->adapter, right, key, &found,
                                                  &right_value) && found &&
                xr_deep_equality_value(ctx, left_value, right_value);
    }
    xr_deep_equality_leave(ctx, already_seen);
    return equal;
}

static inline bool xr_deep_equality_set(XrDeepEqualityContext *ctx, XrValue left, XrValue right,
                                        const XrDeepEqualityNode *left_node,
                                        const XrDeepEqualityNode *right_node) {
    if (!ctx->ops->set_entry || !ctx->ops->set_contains_key_equivalent ||
        left_node->logical_count != right_node->logical_count)
        return false;
    bool already_seen = false;
    if (!xr_deep_equality_enter(ctx, left_node, right_node, &already_seen))
        return false;
    bool equal = true;
    for (uint32_t slot = 0; !already_seen && equal && slot < left_node->iteration_extent; slot++) {
        bool present = false;
        bool contains = false;
        XrValue value = {0};
        equal = ctx->ops->set_entry(ctx->adapter, left, slot, &present, &value);
        if (!equal || !present)
            continue;
        equal = ctx->ops->set_contains_key_equivalent(ctx->adapter, right, value, &contains) &&
                contains;
    }
    xr_deep_equality_leave(ctx, already_seen);
    return equal;
}

static inline bool xr_deep_equality_struct(XrDeepEqualityContext *ctx, XrValue left,
                                           XrValue right, const XrDeepEqualityNode *left_node,
                                           const XrDeepEqualityNode *right_node) {
    if (!ctx->ops->struct_field_pair || left_node->nominal_identity != right_node->nominal_identity ||
        left_node->logical_count != right_node->logical_count)
        return false;
    bool already_seen = false;
    if (!xr_deep_equality_enter(ctx, left_node, right_node, &already_seen))
        return false;
    bool equal = true;
    for (uint32_t i = 0; !already_seen && equal && i < left_node->logical_count; i++) {
        XrValue left_value = {0};
        XrValue right_value = {0};
        equal = ctx->ops->struct_field_pair(ctx->adapter, left, right, i, &left_value,
                                            &right_value) &&
                xr_deep_equality_value(ctx, left_value, right_value);
    }
    xr_deep_equality_leave(ctx, already_seen);
    return equal;
}

static inline bool xr_deep_equality_value(XrDeepEqualityContext *ctx, XrValue left,
                                          XrValue right) {
    if (!ctx || !ctx->ops || !ctx->ops->describe || !ctx->ops->fallback_equal)
        return false;
    XrDeepEqualityNode left_node = {0};
    XrDeepEqualityNode right_node = {0};
    if (!ctx->ops->describe(ctx->adapter, left, &left_node) ||
        !ctx->ops->describe(ctx->adapter, right, &right_node) ||
        left_node.kind <= XR_DEEP_EQUALITY_INVALID ||
        left_node.kind >= XR_DEEP_EQUALITY_KIND_COUNT || left_node.kind != right_node.kind)
        return false;
    if (left_node.kind == XR_DEEP_EQUALITY_IDENTITY)
        return ctx->ops->fallback_equal(ctx->adapter, left, right);
    if (left_node.kind == XR_DEEP_EQUALITY_STRING)
        return ctx->ops->string_equal && ctx->ops->string_equal(ctx->adapter, left, right);
    if (left_node.identity == right_node.identity)
        return true;
    switch (left_node.kind) {
        case XR_DEEP_EQUALITY_ARRAY:
        case XR_DEEP_EQUALITY_TUPLE:
            return xr_deep_equality_sequence(ctx, left, right, &left_node, &right_node);
        case XR_DEEP_EQUALITY_MAP:
            return xr_deep_equality_map(ctx, left, right, &left_node, &right_node);
        case XR_DEEP_EQUALITY_SET:
            return xr_deep_equality_set(ctx, left, right, &left_node, &right_node);
        case XR_DEEP_EQUALITY_STRUCT_OBJECT:
            return xr_deep_equality_struct(ctx, left, right, &left_node, &right_node);
        case XR_DEEP_EQUALITY_DERIVED_INSTANCE:
            if (left_node.nominal_identity != right_node.nominal_identity)
                return false;
            return xr_deep_equality_sequence(ctx, left, right, &left_node, &right_node);
        case XR_DEEP_EQUALITY_ENUM:
            if (left_node.nominal_identity != right_node.nominal_identity ||
                left_node.ordinal != right_node.ordinal)
                return false;
            return xr_deep_equality_sequence(ctx, left, right, &left_node, &right_node);
        default:
            return false;
    }
}

static inline bool xr_deep_equality_apply(const XrDeepEqualityOps *ops, void *adapter,
                                          XrValue left, XrValue right) {
    XrDeepEqualityContext ctx = {.ops = ops, .adapter = adapter};
    return xr_deep_equality_value(&ctx, left, right);
}

#endif /* XR_DEEP_EQUALITY_H */
