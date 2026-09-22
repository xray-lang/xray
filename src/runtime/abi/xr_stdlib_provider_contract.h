/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_contract.h - Declared stdlib provider contracts
 *
 * KEY CONCEPT:
 *   Generated records keep logical semantics separate from an explicitly
 *   selected typed host projection. Lookup uses complete operation identity.
 */

#ifndef XR_STDLIB_PROVIDER_CONTRACT_H
#define XR_STDLIB_PROVIDER_CONTRACT_H

#include "xr_provider_logical_contract.h"

typedef enum XrStdlibProviderAdapter {
    XR_STDLIB_PROVIDER_I64_NULLARY_U64 = 1,
    XR_STDLIB_PROVIDER_I64_NULLARY_I64 = 2,
    XR_STDLIB_PROVIDER_I64_UNARY_STATUS_OUT = 3,
    XR_STDLIB_PROVIDER_OPTIONAL_I64_PAIR_PIPE_CREATE = 4,
    XR_STDLIB_PROVIDER_BOOL_I64_PIPE_CLOSE = 5,
    XR_STDLIB_PROVIDER_TYPED = 6,
} XrStdlibProviderAdapter;

typedef struct XrStdlibProviderDescriptor {
    const char *symbol;
    const char *contract_key;
    const char *operation_key;
    XrStableId contract_id;
    XrStableId operation_id;
    XrFingerprint logical_fingerprint;
    const uint8_t *logical_bytes;
    uint16_t logical_size;
    uint8_t adapter;
    const char *host_header;
    const char *host_symbol;
} XrStdlibProviderDescriptor;

/* Returned descriptors and their member views borrow immutable static storage. */
XR_FUNC size_t xr_stdlib_provider_count(void);
XR_FUNC const XrStdlibProviderDescriptor *xr_stdlib_provider_at(size_t index);
XR_FUNC const XrStdlibProviderDescriptor *xr_stdlib_provider_find(XrStableId contract_id,
                                                                  XrStableId operation_id);
/* Decoding verifies both canonical structure and the generated fingerprint.
 * Failure does not publish a partial contract into out. */
XR_FUNC bool xr_stdlib_provider_logical(const XrStdlibProviderDescriptor *descriptor,
                                        XrProviderLogicalContract *out);

#endif  // XR_STDLIB_PROVIDER_CONTRACT_H
