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

enum {
    XR_STORAGE_DEEP_READONLY = 1u << 0,
    XR_STORAGE_SHARE_SAFE = 1u << 1,
    XR_STORAGE_CONTAINS_EXEC_LOCAL_REF = 1u << 2,
    XR_STORAGE_CONTAINS_BORROW = 1u << 3,
    XR_STORAGE_CONTAINS_FOREIGN_REF = 1u << 4,
    XR_STORAGE_REQUIRES_DROP = 1u << 5,
};

#endif /* XR_STORAGE_H */
