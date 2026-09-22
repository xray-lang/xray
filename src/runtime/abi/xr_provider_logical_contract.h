/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_provider_logical_contract.h - Target-neutral provider operation semantics
 *
 * KEY CONCEPT:
 *   A logical contract describes language values and observable behavior.
 *   Pointer width, C calling convention, and host entry points belong to its
 *   separate target projection.
 */

#ifndef XR_PROVIDER_LOGICAL_CONTRACT_H
#define XR_PROVIDER_LOGICAL_CONTRACT_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_PROVIDER_LOGICAL_SCHEMA_VERSION UINT32_C(1)
#define XR_PROVIDER_LOGICAL_MAX_PARAMETERS 8u
#define XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES 64u
#define XR_PROVIDER_LOGICAL_MAX_RESOURCES 4u
#define XR_PROVIDER_LOGICAL_MAX_PATH 4u
#define XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES 256u

/* Types use a prefix encoding: tuple is followed by its arity and children;
 * optional is followed by its payload. The signature stores parameter types
 * in source order, followed by the result type and typed error type. Unit in
 * the error position means there is no typed error exit. */
typedef enum XrProviderLogicalTypeToken {
    XR_PROVIDER_TYPE_UNIT = 1,
    XR_PROVIDER_TYPE_BOOL = 2,
    XR_PROVIDER_TYPE_I64 = 3,
    XR_PROVIDER_TYPE_BYTES = 4,
    XR_PROVIDER_TYPE_TUPLE = 5,
    XR_PROVIDER_TYPE_OPTIONAL = 6,
    /* Followed by one nonzero, target-independent XrStableId. */
    XR_PROVIDER_TYPE_RESOURCE = 7,
} XrProviderLogicalTypeToken;

typedef enum XrProviderLogicalParameterMode {
    XR_PROVIDER_MODE_IN = 1,
    XR_PROVIDER_MODE_REF = 2,
    XR_PROVIDER_MODE_OUT = 3,
} XrProviderLogicalParameterMode;

typedef enum XrProviderLogicalOwnership {
    XR_PROVIDER_OWNER_TRIVIAL = 1,
    XR_PROVIDER_OWNER_BORROWED = 2,
    XR_PROVIDER_OWNER_CONSUMED = 3,
    XR_PROVIDER_OWNER_OWNED = 4,
} XrProviderLogicalOwnership;

typedef enum XrProviderLogicalEffect {
    XR_PROVIDER_EFFECT_MAY_ERROR = UINT32_C(1) << 0,
    XR_PROVIDER_EFFECT_MAY_PANIC = UINT32_C(1) << 1,
    XR_PROVIDER_EFFECT_MAY_SUSPEND = UINT32_C(1) << 2,
    XR_PROVIDER_EFFECT_READS_CLOCK = UINT32_C(1) << 3,
    XR_PROVIDER_EFFECT_READS_PROCESS = UINT32_C(1) << 4,
    XR_PROVIDER_EFFECT_READS_ENVIRONMENT = UINT32_C(1) << 5,
    XR_PROVIDER_EFFECT_IO = UINT32_C(1) << 6,
    XR_PROVIDER_EFFECT_MANAGED_ALLOCATION = UINT32_C(1) << 7,
    XR_PROVIDER_EFFECT_MANAGED_DEALLOCATION = UINT32_C(1) << 8,
} XrProviderLogicalEffect;

#define XR_PROVIDER_LOGICAL_EFFECTS_ALL ((UINT32_C(1) << 9) - UINT32_C(1))

typedef enum XrProviderLogicalThreadPolicy {
    XR_PROVIDER_THREADS_ANY = 1,
    XR_PROVIDER_THREADS_INSTANCE_AFFINE = 2,
} XrProviderLogicalThreadPolicy;

typedef enum XrProviderLogicalReentryPolicy {
    XR_PROVIDER_REENTRY_ALLOWED = 1,
    XR_PROVIDER_REENTRY_FORBIDDEN = 2,
} XrProviderLogicalReentryPolicy;

typedef enum XrProviderLogicalCallbackPolicy {
    XR_PROVIDER_CALLBACK_NONE = 1,
    XR_PROVIDER_CALLBACK_SYNCHRONOUS = 2,
} XrProviderLogicalCallbackPolicy;

typedef enum XrProviderLogicalRefusalPolicy {
    /* Refusal has no normal result and enters the provider-failed trap edge.
     * Resource consumption follows the separately declared transition point;
     * this policy does not promise that the host operation has not run. */
    XR_PROVIDER_REFUSAL_TRAP = 1,
} XrProviderLogicalRefusalPolicy;

typedef enum XrProviderLogicalPlatform {
    XR_PROVIDER_PLATFORM_LINUX = UINT32_C(1) << 0,
    XR_PROVIDER_PLATFORM_MACOS = UINT32_C(1) << 1,
    XR_PROVIDER_PLATFORM_WINDOWS = UINT32_C(1) << 2,
    XR_PROVIDER_PLATFORM_WASI = UINT32_C(1) << 3,
    XR_PROVIDER_PLATFORM_FREESTANDING = UINT32_C(1) << 4,
} XrProviderLogicalPlatform;

#define XR_PROVIDER_LOGICAL_PLATFORMS_ALL UINT32_C(31)
#define XR_PROVIDER_LOGICAL_PROFILE_HOSTED UINT8_C(1)
#define XR_PROVIDER_LOGICAL_PROFILE_FREESTANDING UINT8_C(2)

typedef enum XrProviderLogicalResourceSource {
    XR_PROVIDER_RESOURCE_PARAMETER = 1,
    XR_PROVIDER_RESOURCE_RESULT = 2,
} XrProviderLogicalResourceSource;

typedef enum XrProviderLogicalResourceAction {
    XR_PROVIDER_RESOURCE_ACQUIRE = 1,
    XR_PROVIDER_RESOURCE_CONSUME = 2,
} XrProviderLogicalResourceAction;

typedef enum XrProviderLogicalResourceTiming {
    XR_PROVIDER_RESOURCE_CALL_ENTER = 1,
    XR_PROVIDER_RESOURCE_RESULT_PRESENT = 2,
} XrProviderLogicalResourceTiming;

/* A result path first selects the optional payload (ordinal zero), then tuple
 * components. Parameter ordinal selects an input before following its path.
 * Resource tokens are independent of the language value's copy ownership. */
typedef struct XrProviderLogicalResourceTransition {
    XrStableId resource_id;
    uint8_t source;
    uint8_t ordinal;
    uint8_t action;
    uint8_t timing;
    uint8_t path_count;
    uint8_t path[XR_PROVIDER_LOGICAL_MAX_PATH];
    uint8_t reserved[3];
} XrProviderLogicalResourceTransition;

typedef struct XrProviderLogicalContract {
    uint32_t schema_version;
    uint32_t effects;
    uint32_t platforms;
    uint8_t runtime_profiles;
    uint8_t parameter_count;
    uint8_t type_byte_count;
    uint8_t resource_count;
    uint8_t result_owner;
    uint8_t error_owner;
    uint8_t threads;
    uint8_t reentry;
    uint8_t callbacks;
    uint8_t refusal;
    uint8_t reserved[2];
    uint8_t parameter_modes[XR_PROVIDER_LOGICAL_MAX_PARAMETERS];
    uint8_t parameter_owners[XR_PROVIDER_LOGICAL_MAX_PARAMETERS];
    uint8_t types[XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES];
    XrProviderLogicalResourceTransition resources[XR_PROVIDER_LOGICAL_MAX_RESOURCES];
} XrProviderLogicalContract;

typedef struct XrProviderLogicalTypeView {
    const uint8_t *bytes;
    uint8_t size;
} XrProviderLogicalTypeView;

/* Structural validity is separate from execution admission. A consumer must
 * also support the declared types, effects, resource and concurrency policies. */
XR_FUNC bool xr_provider_logical_contract_verify(const XrProviderLogicalContract *contract);
/* An absent logical contract is reserved for non-language runtime foundations.
 * Invalid contracts never compare equal, including two absent contracts. */
XR_FUNC bool xr_provider_logical_contract_is_zero(const XrProviderLogicalContract *contract);
XR_FUNC bool xr_provider_logical_contract_equal(const XrProviderLogicalContract *left,
                                                const XrProviderLogicalContract *right);
XR_FUNC bool xr_provider_logical_contract_fingerprint(const XrProviderLogicalContract *contract,
                                                      XrFingerprint *out);
/* Type views borrow contract storage. Indexes below parameter_count select
 * inputs; the next two indexes select the result and typed error. */
XR_FUNC bool xr_provider_logical_contract_type(const XrProviderLogicalContract *contract,
                                               uint8_t index, XrProviderLogicalTypeView *out);
/* A resource leaf carries its exact nominal resource identity, never a host
 * handle, pointer, layout or destructor. Failure leaves the output unchanged. */
XR_FUNC bool xr_provider_logical_resource_type_id(XrProviderLogicalTypeView type,
                                                  XrStableId *out);
/* Failed encoding/decoding leaves every output unchanged. Encoded bytes have
 * a canonical field order and no C padding or target ABI representation. */
XR_FUNC bool xr_provider_logical_contract_encode(const XrProviderLogicalContract *contract,
                                                 uint8_t *out, size_t capacity, size_t *size_out);
XR_FUNC bool xr_provider_logical_contract_decode(const uint8_t *bytes, size_t size,
                                                 XrProviderLogicalContract *out);

#endif
