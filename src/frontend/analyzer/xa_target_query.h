/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_target_query.h - Compiler-owned target query identities
 *
 * KEY CONCEPT:
 *   Target queries are recognized from a compiler-owned namespace binding and
 *   published as pointer-free semantic facts. Source spellings never cross the
 *   analyzer boundary and user declarations cannot acquire these identities.
 */

#ifndef XA_TARGET_QUERY_H
#define XA_TARGET_QUERY_H

#include <stdint.h>

typedef enum XaTargetNamespaceId {
    XA_TARGET_NAMESPACE_NONE = 0,
    XA_TARGET_NAMESPACE_TARGET = 1,
} XaTargetNamespaceId;

typedef enum XaTargetQueryId {
    XA_TARGET_QUERY_NONE = 0,
    XA_TARGET_QUERY_POINTER_BITS = 1,
    XA_TARGET_QUERY_OPERATING_SYSTEM = 2,
    XA_TARGET_QUERY_ARCHITECTURE = 3,
    XA_TARGET_QUERY_NATIVE_ABI = 4,
    XA_TARGET_QUERY_ENDIANNESS = 5,
} XaTargetQueryId;

typedef struct XaTargetQueryFact {
    uint16_t namespace_id;      /* XaTargetNamespaceId */
    uint16_t query_id;          /* XaTargetQueryId */
    uint8_t result_native_type; /* XrNativeType */
    uint8_t complete;
} XaTargetQueryFact;

typedef struct XaNodeTargetQueryEntry {
    uint32_t node_id;
    XaTargetQueryFact fact;
} XaNodeTargetQueryEntry;

#endif  // XA_TARGET_QUERY_H
