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

#define XR_PROVIDER_IO_CONTRACT_KEY "xray.runtime.provider.v1/io"
#define XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY                                               \
    "xray.runtime.provider-operation.v1/io/output-write"
#define XR_PROVIDER_IO_ASSERTION_REPORT_OPERATION_KEY                                           \
    "xray.runtime.provider-operation.v1/io/assertion-report"

#endif /* XR_BUILTIN_PROVIDER_CONTRACT_H */
