/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile.h - Immutable backend-neutral target facts
 *
 * KEY CONCEPT:
 *   A target profile is an exact, immutable value. It contains numeric ABI,
 *   runtime, and provider identities but no backend spelling or compiler
 *   object, so every executor can compare the same fingerprint.
 */

#ifndef XR_TARGET_PROFILE_H
#define XR_TARGET_PROFILE_H

#include "../semantic/xr_semantic_ids.h"
#include "../../runtime/abi/xr_runtime_contract.h"
#include "../../runtime/abi/xr_runtime_object_header.h"
#include "../../runtime/abi/xr_runtime_string_object.h"
#include "../../runtime/abi/xr_target_machine_facts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TARGET_PROFILE_SCHEMA_VERSION UINT32_C(5)
#define XR_TARGET_PLAN_SCHEMA_VERSION UINT32_C(58)
#define XR_BOUNDARY_ABI_SCHEMA_VERSION UINT32_C(2)
#define XR_RUNTIME_KERNEL_SCHEMA_VERSION UINT32_C(1)
#define XR_BOUNDARY_ABI_VALUE_COUNT UINT8_C(5)

typedef struct XrTargetProfile XrTargetProfile;
typedef XrFingerprint XrTargetProfileId;
typedef XrFingerprint XrTargetSemanticsId;
typedef XrFingerprint XrBoundaryAbiId;
typedef XrFingerprint XrRuntimeKernelId;
typedef XrFingerprint XrProviderContractSetId;

typedef enum XrBoundaryValueRepresentation {
    XR_BOUNDARY_VALUE_VOID = 0,
    XR_BOUNDARY_VALUE_CANONICAL_BOOL,
    XR_BOUNDARY_VALUE_TWOS_COMPLEMENT_INTEGER,
    XR_BOUNDARY_VALUE_UNSIGNED_INTEGER,
    XR_BOUNDARY_VALUE_TYPED_ERROR_CODE,
} XrBoundaryValueRepresentation;

typedef enum XrBoundaryOwnership {
    XR_BOUNDARY_OWNERSHIP_NONE = 0,
    XR_BOUNDARY_OWNERSHIP_COPY,
} XrBoundaryOwnership;

typedef enum XrBoundaryCallConvention {
    XR_BOUNDARY_CALL_INVALID = 0,
    XR_BOUNDARY_CALL_FRAME_V1,
} XrBoundaryCallConvention;

typedef enum XrBoundaryErrorModel {
    XR_BOUNDARY_ERROR_INVALID = 0,
    XR_BOUNDARY_ERROR_TYPED_CODE,
} XrBoundaryErrorModel;

typedef enum XrBoundaryAggregateLayoutModel {
    XR_BOUNDARY_AGGREGATE_LAYOUT_INVALID = 0,
    XR_BOUNDARY_AGGREGATE_LAYOUT_DECLARATION_ORDER_NATURAL,
} XrBoundaryAggregateLayoutModel;

typedef enum XrBoundaryVariantLayoutModel {
    XR_BOUNDARY_VARIANT_LAYOUT_INVALID = 0,
    XR_BOUNDARY_VARIANT_LAYOUT_U32_TAG_NATURAL_PAYLOAD,
} XrBoundaryVariantLayoutModel;

typedef enum XrBoundaryRootModel {
    XR_BOUNDARY_ROOT_MODEL_INVALID = 0,
    XR_BOUNDARY_ROOT_MODEL_EXPLICIT_OFFSETS,
} XrBoundaryRootModel;

typedef enum XrBoundaryCleanupModel {
    XR_BOUNDARY_CLEANUP_MODEL_INVALID = 0,
    XR_BOUNDARY_CLEANUP_MODEL_EXPLICIT_ACTIONS,
} XrBoundaryCleanupModel;

typedef struct XrBoundaryValueAbi {
    uint16_t type_id;
    uint8_t representation;
    uint8_t ownership;
    uint16_t size;
    uint16_t alignment;
} XrBoundaryValueAbi;

typedef struct XrBoundaryAbi {
    uint32_t schema_version;
    uint8_t target_endian;
    uint8_t pointer_size;
    uint8_t pointer_alignment;
    uint8_t value_count;
    uint8_t call_convention;
    uint8_t error_model;
    uint8_t coroutine_model;
    uint8_t aggregate_layout_model;
    uint8_t variant_layout_model;
    uint8_t root_model;
    uint8_t cleanup_model;
    uint16_t variant_tag_type;
    uint16_t reserved16;
    XrBoundaryValueAbi values[XR_BOUNDARY_ABI_VALUE_COUNT];
    XrFingerprint object_header_id;
    XrFingerprint string_object_id;
    XrBoundaryAbiId id;
} XrBoundaryAbi;

typedef enum XrRuntimeKernelPolicy {
    XR_RUNTIME_KERNEL_POLICY_INVALID = 0,
    XR_RUNTIME_KERNEL_POLICY_CONTRACTUAL,
} XrRuntimeKernelPolicy;

typedef struct XrRuntimeKernelContract {
    uint32_t schema_version;
    uint8_t runtime_profile;
    uint8_t rc_policy;
    uint8_t weak_policy;
    uint8_t generation_protocol;
    uint8_t panic_policy;
    uint8_t oom_policy;
    uint16_t reserved16;
    uint64_t scheduler_hook_mask;
    XrFingerprint runtime_abi_id;
    XrRuntimeKernelId id;
} XrRuntimeKernelContract;

/* Numeric codegen selection after backend option resolution. This is not a
 * target identity; the production authority validates it against one. */
typedef struct XrTargetCodegenFacts {
    uint64_t vector_feature_mask;
    uint16_t maximum_vector_bits;
    uint16_t reserved16;
    uint32_t reserved32;
} XrTargetCodegenFacts;

/* Structured inputs are consumed during the call and are never retained. */
typedef struct XrTargetProfileBuildInput {
    XrTargetMachineFacts machine;
    const XrRuntimeAbiContract *runtime_abi;
    const XrRuntimeObjectHeaderMaterializationFacts *object_header_materialization;
    const XrRuntimeStringObjectContract *string_contract;
    const XrTargetProviderContract *providers;
    size_t provider_count;
} XrTargetProfileBuildInput;

XR_FUNC bool xr_target_profile_build(const XrTargetProfileBuildInput *input, XrTargetProfile **out,
                                     char *error, size_t error_size);
XR_FUNC bool xr_target_profile_require_exact(const XrTargetProfile *expected,
                                             const XrTargetProfile *actual, char *error,
                                             size_t error_size);
XR_FUNC XrTargetProfile *xr_target_profile_retain(const XrTargetProfile *profile);
XR_FUNC void xr_target_profile_free(XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_is_frozen(const XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_verify(const XrTargetProfile *profile, char *error,
                                      size_t error_size);
XR_FUNC XrFingerprint xr_target_profile_fingerprint(const XrTargetProfile *profile);
XR_FUNC const XrTargetMachineFacts *xr_target_profile_machine_facts(const XrTargetProfile *profile);
XR_FUNC XrTargetSemanticsId xr_target_profile_target_semantics_id(const XrTargetProfile *profile);
XR_FUNC const XrBoundaryAbi *xr_target_profile_boundary_abi(const XrTargetProfile *profile);
XR_FUNC const XrRuntimeKernelContract *
xr_target_profile_runtime_kernel(const XrTargetProfile *profile);
XR_FUNC XrProviderContractSetId
xr_target_profile_provider_contract_set_id(const XrTargetProfile *profile);
XR_FUNC size_t xr_target_profile_provider_count(const XrTargetProfile *profile);
XR_FUNC const XrTargetProviderContract *xr_target_profile_provider(const XrTargetProfile *profile,
                                                                   size_t index);

#endif  // XR_TARGET_PROFILE_H
