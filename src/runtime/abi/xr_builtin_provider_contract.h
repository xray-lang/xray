/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_builtin_provider_contract.h - Canonical logical provider identities
 *
 * These keys are target-neutral program facts.  Hosted and freestanding
 * profiles may realize them with different concrete ABIs and fingerprints,
 * but an XrProgram must never encode a backend-specific provider name.
 */

#ifndef XR_BUILTIN_PROVIDER_CONTRACT_H
#define XR_BUILTIN_PROVIDER_CONTRACT_H

#include "xr_stdlib_provider_keys_gen.h"
#include "xr_provider_logical_contract.h"

#define XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY                                               \
    "xray.runtime.provider-operation.v1/io/output-write"
#define XR_PROVIDER_IO_ASSERTION_REPORT_OPERATION_KEY                                           \
    "xray.runtime.provider-operation.v1/io/assertion-report"

/* Core output and assertion reporting borrow one byte span for the duration
 * of a call. Rendering and panic publication belong to their CoreSpec ops. */
static inline XrProviderLogicalContract xr_builtin_provider_byte_sink_logical_contract(void) {
    return (XrProviderLogicalContract) {
        .schema_version = XR_PROVIDER_LOGICAL_SCHEMA_VERSION,
        .effects = XR_PROVIDER_EFFECT_IO,
        .platforms = XR_PROVIDER_LOGICAL_PLATFORMS_ALL,
        .runtime_profiles =
            XR_PROVIDER_LOGICAL_PROFILE_HOSTED | XR_PROVIDER_LOGICAL_PROFILE_FREESTANDING,
        .parameter_count = 1u,
        .type_byte_count = 3u,
        .result_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .error_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .threads = XR_PROVIDER_THREADS_ANY,
        .reentry = XR_PROVIDER_REENTRY_ALLOWED,
        .callbacks = XR_PROVIDER_CALLBACK_NONE,
        .refusal = XR_PROVIDER_REFUSAL_TRAP,
        .parameter_modes = {XR_PROVIDER_MODE_IN},
        .parameter_owners = {XR_PROVIDER_OWNER_BORROWED},
        .types = {XR_PROVIDER_TYPE_BYTES, XR_PROVIDER_TYPE_UNIT, XR_PROVIDER_TYPE_UNIT},
    };
}
#endif /* XR_BUILTIN_PROVIDER_CONTRACT_H */
