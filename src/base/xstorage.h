/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstorage.h - Canonical storage provenance shared by analysis and runtimes.
 */

#ifndef XR_STORAGE_H
#define XR_STORAGE_H

#include <stdint.h>

typedef enum XrStorageOwner {
    XR_STORAGE_NONE = 0,
    XR_STORAGE_EXEC_LOCAL,
    XR_STORAGE_MODULE,
    XR_STORAGE_SHARED_SYSTEM,
    XR_STORAGE_FOREIGN,
} XrStorageOwner;

typedef enum XrStorageMutability {
    XR_STORAGE_READONLY = 0,
    XR_STORAGE_MUTABLE,
    XR_STORAGE_INTERIOR_MUTABLE,
} XrStorageMutability;

typedef enum XrAddressIdentity {
    XR_ADDRESS_NONE = 0,
    XR_ADDRESS_LEXICAL,
    XR_ADDRESS_MODULE_STABLE,
    XR_ADDRESS_SHARED_STABLE,
    XR_ADDRESS_FOREIGN,
} XrAddressIdentity;

typedef enum XrStorageMaterialization {
    XR_MATERIALIZE_INLINE = 0,
    XR_MATERIALIZE_EXEC_LOCAL,
    XR_MATERIALIZE_MODULE_READONLY,
    XR_MATERIALIZE_MODULE_RUNTIME,
    XR_MATERIALIZE_SHARED_SYSTEM,
    XR_MATERIALIZE_REJECT,
} XrStorageMaterialization;

typedef enum XrCaptureAction {
    XR_CAPTURE_INLINE_VALUE = 0,
    XR_CAPTURE_DEEP_COPY,
    XR_CAPTURE_MOVE,
    XR_CAPTURE_MODULE_READONLY,
    XR_CAPTURE_SHARED_REF,
    XR_CAPTURE_REJECT,
} XrCaptureAction;

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
    uint8_t owner;
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
