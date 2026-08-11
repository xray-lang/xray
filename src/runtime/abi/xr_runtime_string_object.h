/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_string_object.h - Canonical materialized UTF-8 string object
 */

#ifndef XR_RUNTIME_STRING_OBJECT_H
#define XR_RUNTIME_STRING_OBJECT_H

#include "xr_runtime_descriptor.h"
#include "xr_runtime_object_header.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#define XR_RUNTIME_STRING_CONTRACT_SCHEMA_VERSION UINT32_C(1)
#define XR_RUNTIME_STRING_FIXED_PREFIX_SIZE UINT32_C(32)
#define XR_RUNTIME_STRING_LAYOUT_INDEX UINT32_C(0)
#define XR_RUNTIME_STRING_DOMAIN_COUNT UINT32_C(4)
#define XR_RUNTIME_STRING_FIELD_COUNT UINT32_C(6)
#define XR_RUNTIME_STRING_MATERIALIZATION_COUNT UINT32_C(2)
#define XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH                              \
    (UINT32_MAX - XR_RUNTIME_STRING_FIXED_PREFIX_SIZE - UINT32_C(1))

typedef enum XrRuntimeStringDomainIndex {
    XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL = 0,
    XR_RUNTIME_STRING_DOMAIN_TRANSFERABLE = 1,
    XR_RUNTIME_STRING_DOMAIN_CONST_SHARED = 2,
    XR_RUNTIME_STRING_DOMAIN_SYNC_SHARED = 3,
} XrRuntimeStringDomainIndex;

typedef enum XrRuntimeStringTraits {
    XR_RUNTIME_STRING_TRAIT_LONG = UINT16_C(1) << 0,
    XR_RUNTIME_STRING_TRAIT_INTERNED = UINT16_C(1) << 1,
    XR_RUNTIME_STRING_TRAIT_GLOBAL = UINT16_C(1) << 2,
    XR_RUNTIME_STRING_TRAIT_LOCAL = UINT16_C(1) << 3,
    XR_RUNTIME_STRING_TRAIT_PERMANENT = UINT16_C(1) << 4,
    XR_RUNTIME_STRING_TRAIT_ACCESSED = UINT16_C(1) << 5,
} XrRuntimeStringTraits;

typedef enum XrRuntimeStringFieldRole {
    XR_RUNTIME_STRING_FIELD_OBJECT_HEADER = 1,
    XR_RUNTIME_STRING_FIELD_TRAITS = 2,
    XR_RUNTIME_STRING_FIELD_BYTE_LENGTH = 3,
    XR_RUNTIME_STRING_FIELD_RUNE_LENGTH = 4,
    XR_RUNTIME_STRING_FIELD_HASH = 5,
    XR_RUNTIME_STRING_FIELD_UTF8_TAIL = 6,
} XrRuntimeStringFieldRole;

typedef enum XrRuntimeStringFieldFlags {
    XR_RUNTIME_STRING_FIELD_ATOMIC = UINT16_C(1) << 0,
    XR_RUNTIME_STRING_FIELD_FLEXIBLE_TAIL = UINT16_C(1) << 1,
} XrRuntimeStringFieldFlags;

typedef struct XrRuntimeStringFieldDescriptor {
    uint16_t role;
    uint16_t flags;
    uint32_t offset;
    uint32_t width;
    uint32_t reserved;
} XrRuntimeStringFieldDescriptor;

typedef enum XrRuntimeStringMaterializationKind {
    XR_RUNTIME_STRING_MATERIALIZATION_LITERAL_VIEW = 1,
    XR_RUNTIME_STRING_MATERIALIZATION_OWNED_OBJECT = 2,
} XrRuntimeStringMaterializationKind;

typedef struct XrRuntimeStringMaterializationDescriptor {
    uint8_t kind;
    uint8_t has_object_header;
    uint8_t owns_utf8_bytes;
    uint8_t nul_terminated;
    uint32_t semantic_domain_mask;
    uint32_t backend_materialization_mask;
    uint32_t reserved;
} XrRuntimeStringMaterializationDescriptor;

#define XR_RUNTIME_STRING_TRAIT_VALID_MASK                                           \
    (XR_RUNTIME_STRING_TRAIT_LONG | XR_RUNTIME_STRING_TRAIT_INTERNED |              \
     XR_RUNTIME_STRING_TRAIT_GLOBAL | XR_RUNTIME_STRING_TRAIT_LOCAL |               \
     XR_RUNTIME_STRING_TRAIT_PERMANENT | XR_RUNTIME_STRING_TRAIT_ACCESSED)

static inline bool xr_runtime_string_traits_valid(uint32_t domain_index,
                                                  uint16_t traits) {
    if ((traits & ~XR_RUNTIME_STRING_TRAIT_VALID_MASK) != 0)
        return false;
    bool local = (traits & XR_RUNTIME_STRING_TRAIT_LOCAL) != 0;
    bool global = (traits & XR_RUNTIME_STRING_TRAIT_GLOBAL) != 0;
    bool interned = (traits & XR_RUNTIME_STRING_TRAIT_INTERNED) != 0;
    bool permanent = (traits & XR_RUNTIME_STRING_TRAIT_PERMANENT) != 0;
    bool accessed = (traits & XR_RUNTIME_STRING_TRAIT_ACCESSED) != 0;
    if (local && (global || interned || permanent || accessed))
        return false;
    if ((permanent || accessed || global) && !interned)
        return false;
    if (domain_index == XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL)
        return local;
    if (domain_index == XR_RUNTIME_STRING_DOMAIN_CONST_SHARED)
        return !local;
    return domain_index < XR_RUNTIME_STRING_DOMAIN_COUNT && !local &&
           !global && !permanent && !accessed;
}

/* Static literals in generated C are non-object views. Every materialized
 * runtime string, including VM, hosted AOT, local, shared, and interned forms,
 * uses this exact header-first shape. */
typedef struct XrString {
    XrRuntimeObjectHeader header;
    _Atomic uint16_t traits;
    uint16_t reserved16;
    uint32_t length;
    uint32_t rune_length;
    uint32_t hash;
    char data[];
} XrString;

_Static_assert(offsetof(XrString, header) == 0,
               "runtime string header must be first");
_Static_assert(offsetof(XrString, traits) == 16,
               "runtime string traits offset drift");
_Static_assert(offsetof(XrString, length) == 20,
               "runtime string length offset drift");
_Static_assert(offsetof(XrString, rune_length) == 24,
               "runtime string rune-length offset drift");
_Static_assert(offsetof(XrString, hash) == 28,
               "runtime string hash offset drift");
_Static_assert(offsetof(XrString, data) == XR_RUNTIME_STRING_FIXED_PREFIX_SIZE,
               "runtime string data offset drift");
_Static_assert(_Alignof(XrString) == XR_RUNTIME_OBJECT_HEADER_ALIGNMENT,
               "runtime string alignment drift");

static inline uint64_t xr_runtime_string_object_allocation_bytes(
    uint32_t byte_length) {
    uint64_t unaligned = (uint64_t) XR_RUNTIME_STRING_FIXED_PREFIX_SIZE +
                         (uint64_t) byte_length + UINT64_C(1);
    uint64_t mask = XR_RUNTIME_OBJECT_HEADER_ALIGNMENT - UINT64_C(1);
    return (unaligned + mask) & ~mask;
}

/* Header-only AOT and hosted runtime allocation paths share this exact
 * materializer. It publishes no pointer and performs no allocation. */
static inline XrRuntimeAbiStatus xr_runtime_string_object_init(
    XrString *string, uint32_t domain_index, uint32_t byte_length,
    uint32_t rune_length, uint32_t hash, uint16_t traits) {
    if (!string)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (domain_index >= XR_RUNTIME_STRING_DOMAIN_COUNT ||
        byte_length > XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH ||
        (rune_length != UINT32_MAX && rune_length > byte_length) ||
        !xr_runtime_string_traits_valid(domain_index, traits))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    atomic_init(&string->header.rc, XR_RUNTIME_OBJECT_RC_INITIAL);
    string->header.object_kind = XR_RUNTIME_OBJECT_KIND_STRING;
    string->header.flags = XR_RUNTIME_OBJECT_FLAG_NONE;
    string->header.layout_id = XR_RUNTIME_STRING_LAYOUT_INDEX;
    string->header.domain_id = domain_index;
    atomic_init(&string->traits, traits);
    string->reserved16 = 0;
    string->length = byte_length;
    string->rune_length = rune_length;
    string->hash = hash;
    return XR_RUNTIME_ABI_OK;
}

typedef struct XrRuntimeStringObjectContract {
    uint32_t schema_version;
    uint32_t layout_index;
    uint32_t domain_count;
    uint32_t data_offset;
    uint32_t extent_operand_bias;
    uint32_t maximum_byte_length;
    uint32_t rune_length_unknown;
    uint16_t object_kind;
    uint16_t trait_valid_mask;
    XrRuntimeStringFieldDescriptor fields[XR_RUNTIME_STRING_FIELD_COUNT];
    XrRuntimeStringMaterializationDescriptor
        materializations[XR_RUNTIME_STRING_MATERIALIZATION_COUNT];
    XrRuntimeExtentDescriptor extent;
    XrRuntimeLayoutDescriptor layout;
    XrRuntimeDomainIdentity domains[XR_RUNTIME_STRING_DOMAIN_COUNT];
    XrFingerprint fingerprint;
    uint64_t reserved[4];
} XrRuntimeStringObjectContract;

XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_contract_build(
    XrRuntimeStringObjectContract *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_contract_fingerprint(
    const XrRuntimeStringObjectContract *contract, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_contract_verify(
    const XrRuntimeStringObjectContract *contract);
XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_extent_for_length(
    const XrRuntimeStringObjectContract *contract, uint32_t byte_length,
    XrRuntimeExtentLimits limits, XrRuntimeEvaluatedExtent *out);

XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_validate_prefix(
    const XrString *string);
XR_FUNC XrRuntimeAbiStatus xr_runtime_string_object_validate(
    const XrString *string, size_t allocation_size);

#endif /* XR_RUNTIME_STRING_OBJECT_H */
