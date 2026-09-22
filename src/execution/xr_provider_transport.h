/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_provider_transport.h - Shared typed host shape and result ownership checks
 */

#ifndef XR_PROVIDER_TRANSPORT_H
#define XR_PROVIDER_TRANSPORT_H
#include "xr_provider_value.h"
#include "../runtime/abi/xr_provider_logical_contract.h"
#include <string.h>

typedef struct XrProviderResourceAccess {
    const void *context;
    bool (*borrow)(const void *, const XrExecutionResource *, XrStableId, void **);
} XrProviderResourceAccess;

static inline bool xr_provider_pack_borrows_payload(const XrProviderValuePack *arguments, const void *payload) {
    for (uint32_t index = 0u; arguments && index < arguments->count; ++index) {
        const XrProviderValueNode *node = &arguments->nodes[index];
        if (node->token == XR_PROVIDER_TYPE_RESOURCE && node->as.resource.payload == payload)
            return true;
    }
    return false;
}

static inline void xr_provider_pack_dispose(XrProviderValuePack *result, const XrProviderValuePack *borrowed,
                                void (*release)(XrExecutionResource **)) {
    if (!result)
        return;
    uint32_t count = result->count < XR_PROVIDER_VALUE_MAX_NODES ? result->count : XR_PROVIDER_VALUE_MAX_NODES;
    result->count = 0u;
    void *released[XR_PROVIDER_VALUE_MAX_NODES] = {0};
    uint32_t released_count = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        XrProviderValueNode node = result->nodes[index];
        memset(&result->nodes[index], 0, sizeof(result->nodes[index]));
        if (node.token != XR_PROVIDER_TYPE_RESOURCE)
            continue;
        if (release && node.as.resource.owner) {
            release(&node.as.resource.owner);
        } else if (node.as.resource.payload && node.as.resource.destroy &&
                   !xr_provider_pack_borrows_payload(borrowed, node.as.resource.payload)) {
            bool duplicate = false;
            for (uint32_t prior = 0u; prior < released_count; ++prior)
                duplicate |= released[prior] == node.as.resource.payload;
            if (!duplicate) {
                released[released_count++] = node.as.resource.payload;
                node.as.resource.destroy(node.as.resource.payload);
            }
        }
    }
}

/* A NULL pack skips an absent optional payload while still consuming its type.
 * Contract validation bounds the entire walk to MAX_TYPE_BYTES logical bytes. */
static inline bool xr_provider_value_walk(const XrProviderResourceAccess *access, XrProviderLogicalTypeView type,
                              size_t *type_offset, XrProviderValuePack *pack,
                              uint32_t *node_offset, bool input) {
    if (*type_offset >= type.size)
        return false;
    uint8_t token = type.bytes[(*type_offset)++];
    XrProviderValueNode *node = NULL;
    if (pack) {
        if (*node_offset >= pack->count)
            return false;
        node = &pack->nodes[(*node_offset)++];
        if (node->token != token || node->reserved16 != 0u)
            return false;
    }
    if (token == XR_PROVIDER_TYPE_TUPLE) {
        if (*type_offset >= type.size)
            return false;
        uint8_t count = type.bytes[(*type_offset)++];
        if (!count || (node && node->child_count != count))
            return false;
        for (uint8_t index = 0u; index < count; ++index) {
            if (!xr_provider_value_walk(access, type, type_offset, pack, node_offset, input))
                return false;
        }
        return true;
    }
    if (token == XR_PROVIDER_TYPE_OPTIONAL) {
        if (node && node->child_count > 1u)
            return false;
        return xr_provider_value_walk(access, type, type_offset,
                                 node && node->child_count ? pack : NULL, node_offset, input);
    }
    if (node && node->child_count != 0u)
        return false;
    if (token == XR_PROVIDER_TYPE_RESOURCE) {
        if (type.size - *type_offset < XR_STABLE_ID_BYTES)
            return false;
        XrStableId id;
        memcpy(id.bytes, type.bytes + *type_offset, XR_STABLE_ID_BYTES);
        *type_offset += XR_STABLE_ID_BYTES;
        if (!node)
            return true;
        if (!(memcmp(node->as.resource.id.bytes, id.bytes, XR_STABLE_ID_BYTES) == 0))
            return false;
        if (input) {
            void *payload = NULL;
            if (node->as.resource.payload || node->as.resource.destroy ||
                !access || !access->borrow ||
                !access->borrow(access->context, node->as.resource.owner, id, &payload))
                return false;
            node->as.resource.payload = payload;
            node->as.resource.owner = NULL;
            return true;
        }
        return !node->as.resource.owner && node->as.resource.payload && node->as.resource.destroy;
    }
    if (token == XR_PROVIDER_TYPE_BYTES)
        return !node || (input && (node->as.bytes.size == 0u || node->as.bytes.data));
    return token == XR_PROVIDER_TYPE_UNIT || token == XR_PROVIDER_TYPE_BOOL || token == XR_PROVIDER_TYPE_I64;
}

static inline bool xr_provider_result_resources_unique(const XrProviderValuePack *result,
                                          const XrProviderValuePack *arguments) {
    for (uint32_t index = 0u; index < result->count; ++index) {
        const XrProviderValueNode *node = &result->nodes[index];
        if (node->token != XR_PROVIDER_TYPE_RESOURCE)
            continue;
        if (xr_provider_pack_borrows_payload(arguments, node->as.resource.payload))
            return false;
        for (uint32_t prior = 0u; prior < index; ++prior) {
            if (result->nodes[prior].token == XR_PROVIDER_TYPE_RESOURCE &&
                result->nodes[prior].as.resource.payload == node->as.resource.payload)
                return false;
        }
    }
    return true;
}

#endif
