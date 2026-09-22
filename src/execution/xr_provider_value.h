/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_provider_value.h - Bounded typed host transport shared with generated C
 */
#ifndef XR_PROVIDER_VALUE_H
#define XR_PROVIDER_VALUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef XSTABLE_ID_H
#include "../base/xstable_id.h"
#endif

typedef enum XrProviderCallStatus {
    XR_PROVIDER_CALL_OK = 0,
    XR_PROVIDER_CALL_FAILED,
    XR_PROVIDER_CALL_OUT_OF_MEMORY,
} XrProviderCallStatus;

typedef struct XrExecutionResource XrExecutionResource;

/* Bounded host transport in logical prefix order, never a Program value format.
 * Tuple/optional nodes precede their children; optional has zero or one child.
 * Input resource nodes name an owner; the boundary supplies its borrowed payload
 * to host code. Host result resources supply payload and destroy, with owner NULL;
 * the boundary validates identity and adopts them before publishing the result.
 * Host callbacks must describe every allocated result even on refusal so cleanup
 * can reclaim it. They may not retain borrowed inputs or return them as owners. */
#define XR_PROVIDER_VALUE_MAX_NODES 64u
typedef struct XrProviderValueNode {
    uint8_t token;
    uint8_t child_count;
    uint16_t reserved16;
    union {
        bool boolean;
        int64_t i64;
        struct { const uint8_t *data; size_t size; } bytes;
        struct {
            XrStableId id;
            void *payload;
            void (*destroy)(void *);
            XrExecutionResource *owner;
        } resource;
    } as;
} XrProviderValueNode;

typedef struct XrProviderValuePack {
    uint32_t count;
    XrProviderValueNode nodes[XR_PROVIDER_VALUE_MAX_NODES];
} XrProviderValuePack;


#endif
