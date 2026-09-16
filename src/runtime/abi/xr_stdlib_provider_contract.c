/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_contract.c - Immutable declared provider contract lookup
 *
 * KEY CONCEPT:
 *   Identity lookup and logical decoding use generated records directly.
 *   Neither host names nor physical calling conventions supply semantics.
 */

#include "xr_stdlib_provider_contract.h"

#include <string.h>

#include "xr_stdlib_provider_descriptors_gen.inc.c"

XR_FUNCDEF size_t xr_stdlib_provider_count(void) {
    return XR_STDLIB_PROVIDER_DESCRIPTOR_COUNT;
}

XR_FUNCDEF const XrStdlibProviderDescriptor *xr_stdlib_provider_at(size_t index) {
    return index < xr_stdlib_provider_count() ? &xr_stdlib_provider_descriptors[index] : NULL;
}

XR_FUNCDEF const XrStdlibProviderDescriptor *xr_stdlib_provider_find(XrStableId contract_id,
                                                                     XrStableId operation_id) {
    size_t low = 0u;
    size_t high = xr_stdlib_provider_count();
    while (low < high) {
        size_t index = low + (high - low) / 2u;
        const XrStdlibProviderDescriptor *candidate = xr_stdlib_provider_at(index);
        int order = memcmp(contract_id.bytes, candidate->contract_id.bytes, XR_STABLE_ID_BYTES);
        if (order == 0)
            order = memcmp(operation_id.bytes, candidate->operation_id.bytes, XR_STABLE_ID_BYTES);
        if (order == 0)
            return candidate;
        if (order < 0)
            high = index;
        else
            low = index + 1u;
    }
    return NULL;
}

XR_FUNCDEF bool xr_stdlib_provider_logical(const XrStdlibProviderDescriptor *descriptor,
                                           XrProviderLogicalContract *out) {
    XrProviderLogicalContract logical;
    XrFingerprint fingerprint;
    if (!descriptor || !out ||
        !xr_provider_logical_contract_decode(descriptor->logical_bytes, descriptor->logical_size,
                                             &logical) ||
        !xr_provider_logical_contract_fingerprint(&logical, &fingerprint) ||
        memcmp(fingerprint.bytes, descriptor->logical_fingerprint.bytes, XR_FINGERPRINT_BYTES) != 0)
        return false;
    *out = logical;
    return true;
}
