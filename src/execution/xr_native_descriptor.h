/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_descriptor.h - Immutable generated native entry descriptor
 */

#ifndef XR_NATIVE_DESCRIPTOR_H
#define XR_NATIVE_DESCRIPTOR_H

#include "xr_execution_identity.h"
#include "xr_provider_value.h"

#define XR_BACKEND_NATIVE_DESCRIPTOR_SCHEMA_VERSION UINT32_C(9)

typedef enum XrBackendExecutionOutcomeKind {
    XR_BACKEND_EXECUTION_RETURN = 0,
    XR_BACKEND_EXECUTION_SUSPENDED,
    XR_BACKEND_EXECUTION_TRAP,
    XR_BACKEND_EXECUTION_CANCELLED,
    XR_BACKEND_EXECUTION_INVALID,
    XR_BACKEND_EXECUTION_INITIALIZING,
    XR_BACKEND_EXECUTION_ERROR,
    XR_BACKEND_EXECUTION_PANIC,
} XrBackendExecutionOutcomeKind;

typedef struct XrBackendExecutionOutcome {
    XrBackendExecutionOutcomeKind kind;
    int64_t value;
    uint32_t state_id;
    uint32_t safepoint_id;
    XrSuspensionRequest suspension;
    /* Original panic cause and optional bounds; code zero remains a valid cause. */
    uint32_t panic_present;
    uint32_t panic_code;
    uint32_t panic_has_bounds;
    int64_t panic_index;
    uint64_t panic_length;
    /* Borrowed message bytes remain valid until the observing execution is freed. */
    uint32_t panic_has_message;
    const uint8_t *panic_message;
    size_t panic_message_size;
    /* Borrowed typed storage, interpreted only by the generated backend, valid
     * until the observing execution is freed. Initializer failures are owned by
     * its leased instance; ordinary entry failures are owned by its frame. */
    uint16_t error_type_id;
    const void *error_value;
} XrBackendExecutionOutcome;

typedef struct XrBackendNativeOutcome {
    uint32_t kind;
    int64_t value;
    uint32_t state_id;
    uint32_t safepoint_id;
    XrSuspensionRequest suspension;
    /* Original panic cause and optional bounds; code zero remains a valid cause. */
    uint32_t panic_present;
    uint32_t panic_code;
    uint32_t panic_has_bounds;
    int64_t panic_index;
    uint64_t panic_length;
    /* Borrowed message bytes remain valid until the observing execution is freed. */
    uint32_t panic_has_message;
    const uint8_t *panic_message;
    size_t panic_message_size;
    uint16_t error_type_id;
    const void *error_value;
} XrBackendNativeOutcome;

// Backend-private adapter for a generated or loaded native step.
// The host owns frame lifecycle; generated code owns CoreSpec semantics.
typedef XrBackendNativeOutcome (*XrBackendNativeStep)(void *frame);
typedef XrBackendNativeOutcome (*XrBackendNativeCancel)(void *frame);
typedef enum XrBackendNativeProviderStatus {
    XR_BACKEND_NATIVE_PROVIDER_OK = 0,
    XR_BACKEND_NATIVE_PROVIDER_FAILED = 1,
    XR_BACKEND_NATIVE_PROVIDER_RESOURCE_LIMIT = 2,
} XrBackendNativeProviderStatus;

typedef struct XrBackendNativeHost {
    void *context;
    int (*output_write)(void *, uint32_t, uint32_t, const uint8_t *, size_t);
    int (*typed_call)(void *, uint32_t, uint32_t, const XrProviderValuePack *, XrProviderValuePack *);
    void (*typed_dispose)(XrProviderValuePack *);
    void (*resource_free)(XrExecutionResource **);
} XrBackendNativeHost;

typedef void (*XrBackendNativeInitialize)(void *frame, void *state,
                                          const XrBackendNativeHost *host);
typedef void (*XrBackendNativeDrop)(void *frame);
typedef XrBackendNativeOutcome (*XrBackendNativeModuleStep)(void *frame, uint32_t function_id,
                                                            uint8_t cancel);

typedef struct XrBackendNativeDescriptor {
    uint32_t schema_version;
    uint32_t reserved32;
    XrExecutionId execution_id;
    size_t frame_size;
    /* Private state is constructed once per adopted instance binding. Racing
     * candidates are
     * destroyed by the host. The layout token and callbacks must
     * remain alive through
     * retirement. initialize copies the temporary host
     * bindings; its context stays valid
     * through the frame's final drop. */
    const void *state_layout;
    size_t state_size;
    XrBackendNativeDrop state_initialize;
    XrBackendNativeDrop state_drop;
    /* Clear published values on failed or abandoned initialization, before the
     * host
     * publishes failure to waiting leases. Final state_drop still follows. */
    XrBackendNativeDrop initialization_abort;
    XrBackendNativeInitialize initialize;
    XrBackendNativeModuleStep module_step;
    XrBackendNativeStep step;
    XrBackendNativeCancel cancel;
    XrBackendNativeDrop drop;
} XrBackendNativeDescriptor;

#endif  // XR_NATIVE_DESCRIPTOR_H
