/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_descriptor.h - Canonical runtime layout and extent contract
 *
 * KEY CONCEPT:
 *   A materialized allocation has one layout descriptor, one storage-domain
 *   identity, and one checked extent formula. Target plans generate these
 *   pointer-free runtime records; they do not duplicate their facts.
 */

#ifndef XR_RUNTIME_DESCRIPTOR_H
#define XR_RUNTIME_DESCRIPTOR_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include "../../base/xstorage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_RUNTIME_ABI_SCHEMA_VERSION UINT32_C(1)
#define XR_RUNTIME_EXTENT_OPERAND_NONE UINT16_MAX
#define XR_RUNTIME_DOMAIN_INSTANCE_INVALID UINT32_C(0)

typedef enum XrRuntimeAbiStatus {
    XR_RUNTIME_ABI_OK = 0,
    XR_RUNTIME_ABI_INVALID_ARGUMENT,
    XR_RUNTIME_ABI_INVALID_SCHEMA,
    XR_RUNTIME_ABI_INVALID_IDENTITY,
    XR_RUNTIME_ABI_INVALID_KIND,
    XR_RUNTIME_ABI_INVALID_ALIGNMENT,
    XR_RUNTIME_ABI_INVALID_DOMAIN,
    XR_RUNTIME_ABI_INVALID_EXTENT,
    XR_RUNTIME_ABI_FINGERPRINT_MISMATCH,
    XR_RUNTIME_ABI_OVERFLOW,
    XR_RUNTIME_ABI_LIMIT_EXCEEDED,
    XR_RUNTIME_ABI_PROVIDER_REQUIRED,
    XR_RUNTIME_ABI_PROVIDER_REJECTED,
    XR_RUNTIME_ABI_INVALID_GROUP,
} XrRuntimeAbiStatus;

#define XR_SEMANTIC_DOMAIN_MASK(domain) (UINT32_C(1) << (uint32_t) (domain))
#define XR_SEMANTIC_DOMAIN_MASK_ALL                                                         \
    (((UINT32_C(1) << ((uint32_t) XR_STORAGE_FOREIGN + UINT32_C(1))) - UINT32_C(1)) &       \
     ~XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_DOMAIN_UNKNOWN))
#define XR_MATERIALIZATION_MASK(materialization)                                            \
    (UINT32_C(1) << (uint32_t) (materialization))
#define XR_MATERIALIZATION_MASK_ALL                                                         \
    (((UINT32_C(1) << ((uint32_t) XR_MATERIALIZE_EXTERNAL + UINT32_C(1))) - UINT32_C(1)) &  \
     ~XR_MATERIALIZATION_MASK(XR_MATERIALIZE_INVALID))

typedef struct XrRuntimeDomainIdentity {
    XrStableId contract_id;
    uint32_t instance_id;
    uint8_t semantic_domain;
    uint8_t materialization;
} XrRuntimeDomainIdentity;

typedef enum XrRuntimeExtentKind {
    XR_RUNTIME_EXTENT_FIXED = 0,
    XR_RUNTIME_EXTENT_INLINE_TAIL = 1,
    XR_RUNTIME_EXTENT_EXTERNAL_BUFFER = 2,
    XR_RUNTIME_EXTENT_MULTI_BUFFER = 3,
    XR_RUNTIME_EXTENT_PROVIDER_DEFINED = 4,
    XR_RUNTIME_EXTENT_KIND_COUNT = 5,
} XrRuntimeExtentKind;

/* One descriptor evaluates one physical allocation. Multi-buffer objects use
 * one descriptor per backing allocation, joined by group_id and part indexes. */
typedef struct XrRuntimeExtentDescriptor {
    uint32_t schema_version;
    XrStableId id;
    XrStableId layout_id;
    XrStableId group_id;
    XrStableId provider_id;
    uint64_t tail_offset;
    uint64_t stride;
    uint16_t operand_index;
    uint16_t part_index;
    uint16_t part_count;
    uint8_t kind;
    XrFingerprint fingerprint;
} XrRuntimeExtentDescriptor;

typedef enum XrRuntimeLayoutFlags {
    XR_LAYOUT_HAS_ROOTS = UINT32_C(1) << 0,
    XR_LAYOUT_HAS_DESTRUCTOR = UINT32_C(1) << 1,
    XR_LAYOUT_HAS_CLONE = UINT32_C(1) << 2,
    XR_LAYOUT_HAS_EQ_HASH = UINT32_C(1) << 3,
} XrRuntimeLayoutFlags;

#define XR_LAYOUT_DESCRIPTOR_FLAGS_ALL                                                        \
    (XR_LAYOUT_HAS_ROOTS | XR_LAYOUT_HAS_DESTRUCTOR | XR_LAYOUT_HAS_CLONE |                   \
     XR_LAYOUT_HAS_EQ_HASH)

typedef struct XrRuntimeLayoutDescriptor {
    uint32_t schema_version;
    XrStableId descriptor_id;
    XrStableId layout_id;
    XrStableId object_kind_id;
    XrStableId extent_id;
    XrStableId root_plan_id;
    XrStableId destructor_id;
    XrStableId clone_id;
    XrStableId eq_hash_id;
    XrFingerprint extent_fingerprint;
    uint64_t fixed_prefix_size;
    uint32_t alignment;
    uint32_t allowed_semantic_domains;
    uint32_t allowed_materializations;
    uint32_t flags;
    XrFingerprint fingerprint;
} XrRuntimeLayoutDescriptor;

typedef struct XrRuntimeExtentLimits {
    uint64_t max_allocation_bytes;
    uint32_t max_alignment;
} XrRuntimeExtentLimits;

typedef struct XrRuntimeEvaluatedExtent {
    XrStableId extent_id;
    XrFingerprint extent_fingerprint;
    uint64_t bytes;
    uint64_t operand;
    uint32_t alignment;
    uint16_t part_index;
    uint16_t part_count;
} XrRuntimeEvaluatedExtent;

/* Ephemeral verifier view. The pointed-to records remain the canonical,
 * pointer-free ABI; this view is never fingerprinted or persisted. */
typedef struct XrRuntimeExtentGroupEntry {
    const XrRuntimeExtentDescriptor *extent;
    const XrRuntimeLayoutDescriptor *layout;
} XrRuntimeExtentGroupEntry;

typedef struct XrRuntimeExtentGroupSummary {
    XrStableId group_id;
    uint16_t part_count;
    XrFingerprint fingerprint;
} XrRuntimeExtentGroupSummary;

typedef XrRuntimeAbiStatus (*XrRuntimeExtentProviderEvaluateFn)(
    XrStableId provider_id, const uint64_t *operands, size_t operand_count,
    uint64_t *out_bytes, void *context);

XR_FUNC XrRuntimeAbiStatus xr_runtime_extent_descriptor_fingerprint(
    const XrRuntimeExtentDescriptor *extent, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_extent_descriptor_verify(
    const XrRuntimeExtentDescriptor *extent, const XrRuntimeLayoutDescriptor *layout);
XR_FUNC XrRuntimeAbiStatus xr_runtime_extent_evaluate(
    const XrRuntimeExtentDescriptor *extent, const XrRuntimeLayoutDescriptor *layout,
    const uint64_t *operands, size_t operand_count, XrRuntimeExtentLimits limits,
    XrRuntimeExtentProviderEvaluateFn provider, void *provider_context,
    XrRuntimeEvaluatedExtent *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_extent_group_verify(
    const XrRuntimeExtentGroupEntry *entries, size_t entry_count,
    XrRuntimeExtentGroupSummary *out);

XR_FUNC XrRuntimeAbiStatus xr_runtime_layout_descriptor_fingerprint(
    const XrRuntimeLayoutDescriptor *layout, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_layout_descriptor_verify(
    const XrRuntimeLayoutDescriptor *layout, const XrRuntimeExtentDescriptor *extent);
XR_FUNC bool xr_runtime_layout_allows_domain(const XrRuntimeLayoutDescriptor *layout,
                                             XrRuntimeDomainIdentity domain);
XR_FUNC bool xr_runtime_domain_identity_valid(XrRuntimeDomainIdentity domain);
XR_FUNC bool xr_runtime_domain_identity_equal(XrRuntimeDomainIdentity left,
                                              XrRuntimeDomainIdentity right);
XR_FUNC const char *xr_runtime_abi_status_name(XrRuntimeAbiStatus status);

#endif  // XR_RUNTIME_DESCRIPTOR_H
