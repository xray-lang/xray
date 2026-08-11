/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_object_header.h - Canonical materialized runtime object header
 *
 * KEY CONCEPT:
 *   Every runtime object that crosses an executor boundary uses this exact
 *   five-field header. Concrete behavior is selected by a verified layout;
 *   executor-private tags and allocation sizes are not header facts.
 */

#ifndef XR_RUNTIME_OBJECT_HEADER_H
#define XR_RUNTIME_OBJECT_HEADER_H

#include "xr_runtime_contract.h"
#include <stdatomic.h>
#include <stdint.h>

#define XR_RUNTIME_OBJECT_HEADER_SIZE 16
#define XR_RUNTIME_OBJECT_HEADER_ALIGNMENT 4
#define XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX UINT32_MAX

#define XR_RUNTIME_OBJECT_RC_INITIAL INT32_C(1)
#define XR_RUNTIME_OBJECT_RC_RETAIN_DELTA INT32_C(1)
#define XR_RUNTIME_OBJECT_RC_RELEASE_DELTA (-INT32_C(1))
#define XR_RUNTIME_OBJECT_RC_STICKY INT32_MIN
#define XR_RUNTIME_OBJECT_RC_STICKY_BAND (INT32_MIN + INT32_C(1024))

typedef enum XrRuntimeObjectKind {
    XR_RUNTIME_OBJECT_KIND_INVALID = 0,
    XR_RUNTIME_OBJECT_KIND_STRING = 1,
    XR_RUNTIME_OBJECT_KIND_CLOSURE = 2,
    XR_RUNTIME_OBJECT_KIND_BOXED_AGGREGATE = 3,
    XR_RUNTIME_OBJECT_KIND_ARRAY = 4,
    XR_RUNTIME_OBJECT_KIND_MAP = 5,
    XR_RUNTIME_OBJECT_KIND_SET = 6,
    XR_RUNTIME_OBJECT_KIND_INSTANCE = 7,
    XR_RUNTIME_OBJECT_KIND_ENUM_BOX = 8,
    XR_RUNTIME_OBJECT_KIND_CELL = 9,
    XR_RUNTIME_OBJECT_KIND_COUNT = 10,
} XrRuntimeObjectKind;

typedef enum XrRuntimeObjectFlags {
    XR_RUNTIME_OBJECT_FLAG_NONE = 0,
} XrRuntimeObjectFlags;

#define XR_RUNTIME_OBJECT_FLAG_VALID_MASK UINT16_C(0)

typedef enum XrRuntimeAtomicOrderCapability {
    XR_RUNTIME_ATOMIC_ORDER_RELAXED = UINT32_C(1) << 0,
    XR_RUNTIME_ATOMIC_ORDER_ACQUIRE = UINT32_C(1) << 1,
    XR_RUNTIME_ATOMIC_ORDER_RELEASE = UINT32_C(1) << 2,
} XrRuntimeAtomicOrderCapability;

#define XR_RUNTIME_OBJECT_HEADER_REQUIRED_ATOMIC_ORDERS                                \
    (XR_RUNTIME_ATOMIC_ORDER_RELAXED | XR_RUNTIME_ATOMIC_ORDER_ACQUIRE |                \
     XR_RUNTIME_ATOMIC_ORDER_RELEASE)

#define XR_RUNTIME_OBJECT_HEADER_FACTS_SCHEMA_VERSION UINT32_C(1)

/* Pointer-free facts produced by a target probe or by the native runtime.
 * The materializer accepts only the exact canonical layout and atomic model. */
typedef struct XrRuntimeObjectHeaderMaterializationFacts {
    uint32_t schema_version;
    uint16_t header_size;
    uint16_t header_alignment;
    uint16_t atomic_i32_size;
    uint16_t atomic_i32_alignment;
    uint16_t uint16_size;
    uint16_t uint16_alignment;
    uint16_t uint32_size;
    uint16_t uint32_alignment;
    uint16_t rc_offset;
    uint16_t object_kind_offset;
    uint16_t flags_offset;
    uint16_t layout_id_offset;
    uint16_t domain_id_offset;
    uint8_t target_endian;
    uint8_t int32_twos_complement;
    uint8_t atomic_i32_lock_free;
    uint8_t atomic_i32_rmw;
    uint32_t atomic_order_mask;
    uint32_t reserved32;
    uint64_t reserved[2];
} XrRuntimeObjectHeaderMaterializationFacts;

typedef struct XrRuntimeObjectHeader {
    _Atomic int32_t rc;
    uint16_t object_kind;
    uint16_t flags;
    uint32_t layout_id;
    uint32_t domain_id;
} XrRuntimeObjectHeader;

_Static_assert(sizeof(_Atomic int32_t) == 4, "canonical RC must be four bytes");
_Static_assert(_Alignof(_Atomic int32_t) == 4, "canonical RC must be four-byte aligned");
_Static_assert(sizeof(XrRuntimeObjectHeader) == XR_RUNTIME_OBJECT_HEADER_SIZE,
               "canonical object header size drift");
_Static_assert(_Alignof(XrRuntimeObjectHeader) == XR_RUNTIME_OBJECT_HEADER_ALIGNMENT,
               "canonical object header alignment drift");
_Static_assert(offsetof(XrRuntimeObjectHeader, rc) == 0, "canonical RC offset drift");
_Static_assert(offsetof(XrRuntimeObjectHeader, object_kind) == 4,
               "canonical object-kind offset drift");
_Static_assert(offsetof(XrRuntimeObjectHeader, flags) == 6,
               "canonical flags offset drift");
_Static_assert(offsetof(XrRuntimeObjectHeader, layout_id) == 8,
               "canonical layout-ID offset drift");
_Static_assert(offsetof(XrRuntimeObjectHeader, domain_id) == 12,
               "canonical domain-ID offset drift");

XR_FUNC XrRuntimeAbiStatus xr_runtime_object_kind_stable_id(
    uint16_t object_kind, XrStableId *out);

XR_FUNC XrRuntimeAbiStatus xr_runtime_object_header_native_materialization_facts(
    XrRuntimeObjectHeaderMaterializationFacts *out);

/* Builds the structured ABI from independently probed target facts. The caller
 * fingerprints the result with xr_runtime_object_header_abi_fingerprint(). */
XR_FUNC XrRuntimeAbiStatus xr_runtime_object_header_abi_materialize(
    const XrRuntimeObjectHeaderMaterializationFacts *facts,
    XrRuntimeObjectHeaderAbi *out);

XR_FUNC XrRuntimeAbiStatus xr_runtime_object_header_init(
    XrRuntimeObjectHeader *header, uint16_t object_kind, uint16_t flags,
    uint32_t layout_id, uint32_t domain_id);
XR_FUNC XrRuntimeAbiStatus xr_runtime_object_header_validate(
    const XrRuntimeObjectHeader *header);

#endif  // XR_RUNTIME_OBJECT_HEADER_H
