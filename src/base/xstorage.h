/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstorage.h - Canonical storage-domain provenance shared by all backends.
 */

#ifndef XR_STORAGE_H
#define XR_STORAGE_H

#include <stdint.h>

/* Semantic ownership/lifetime domain. This axis never encodes whether a
 * backend chose stack, static data, or a particular heap implementation. */
typedef enum XrSemanticStorageDomain {
    XR_STORAGE_DOMAIN_UNKNOWN = 0,
    XR_STORAGE_EXEC_LOCAL,
    XR_STORAGE_TRANSFERABLE,
    XR_STORAGE_CONST_SHARED,
    XR_STORAGE_SYNC_SHARED,
    XR_STORAGE_MODULE_STATIC,
    XR_STORAGE_FOREIGN,
} XrSemanticStorageDomain;

typedef enum XrStorageMutability {
    XR_STORAGE_READONLY = 0,
    XR_STORAGE_MUTABLE,
    XR_STORAGE_INTERIOR_MUTABLE,
} XrStorageMutability;

typedef enum XrAddressIdentity {
    XR_ADDRESS_NONE = 0,
    XR_ADDRESS_LEXICAL,
    XR_ADDRESS_MODULE_STABLE,
    XR_ADDRESS_SYSTEM_STABLE,
    XR_ADDRESS_FOREIGN,
} XrAddressIdentity;

/* Backend representation. Changing this axis must not grant a stronger
 * semantic domain or transfer capability. */
typedef enum XrBackendMaterialization {
    XR_MATERIALIZE_INVALID = 0,
    XR_MATERIALIZE_INLINE,
    XR_MATERIALIZE_STACK,
    XR_MATERIALIZE_STATIC_DATA,
    XR_MATERIALIZE_EXEC_HEAP,
    XR_MATERIALIZE_SYSTEM_HEAP,
    XR_MATERIALIZE_SROA,
    XR_MATERIALIZE_EXTERNAL,
} XrBackendMaterialization;

/* Canonical action for closure capture and every cross-execution boundary.
 * EXPLICIT_COPY is emitted only for source `copy(...)`; there is no implicit
 * deep-copy fallback. */
typedef enum XrTransferAction {
    XR_TRANSFER_ACTION_INVALID = 0,
    XR_TRANSFER_INLINE_COPY,
    XR_TRANSFER_CONST_SHARE,
    XR_TRANSFER_SYNC_SHARE,
    XR_TRANSFER_MOVE_UNIQUE,
    XR_TRANSFER_EXPLICIT_COPY,
    XR_TRANSFER_MODULE_READ,
    XR_TRANSFER_REJECT,
} XrTransferAction;

typedef enum XrPointerOrigin {
    XR_POINTER_ORIGIN_NONE = 0,
    XR_POINTER_ORIGIN_NULL,
    XR_POINTER_ORIGIN_STATIC,
    XR_POINTER_ORIGIN_MODULE,
    XR_POINTER_ORIGIN_STACK_BORROW,
    XR_POINTER_ORIGIN_OWNER_BORROW,
    XR_POINTER_ORIGIN_FOREIGN,
} XrPointerOrigin;

typedef enum XrPointerEscape {
    XR_POINTER_ESCAPE_NONE = 0,
    XR_POINTER_ESCAPE_LEXICAL,
    XR_POINTER_ESCAPE_CALL_BOUND,
    XR_POINTER_ESCAPE_STABLE,
} XrPointerEscape;

/* Canonical address proof carried by verified backend plans.  A pointer type
 * alone never grants addressability or escape permission. */
typedef struct XrAddressProvenance {
    uint32_t storage_id;
    uint32_t lifetime_id;
    uint8_t domain;
    uint8_t mutability;
    uint8_t address_identity;
    uint8_t origin;
    uint8_t escape;
} XrAddressProvenance;

enum {
    XR_STORAGE_DEEP_READONLY = 1u << 0,
    XR_STORAGE_SHARE_SAFE = 1u << 1,
    XR_STORAGE_CONTAINS_EXEC_LOCAL_REF = 1u << 2,
    XR_STORAGE_CONTAINS_BORROW = 1u << 3,
    XR_STORAGE_CONTAINS_FOREIGN_REF = 1u << 4,
    XR_STORAGE_REQUIRES_DROP = 1u << 5,
};

#endif /* XR_STORAGE_H */
