/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_random_provider.h - Synchronous platform entropy into an executor-owned view
 */
#ifndef XR_RANDOM_PROVIDER_H
#define XR_RANDOM_PROVIDER_H
#include "xr_provider_value.h"
#include "../runtime/abi/xr_provider_logical_contract.h"
#include "../os/os_random.h"

static inline XrProviderCallStatus xr_random_provider_fill(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    if (!arguments || !result || result->count || arguments->count != 1u ||
        arguments->nodes[0].token != XR_PROVIDER_TYPE_U8_ARRAY || arguments->nodes[0].child_count ||
        arguments->nodes[0].reserved16 ||
        (arguments->nodes[0].as.u8_array.size && !arguments->nodes[0].as.u8_array.data))
        return XR_PROVIDER_CALL_FAILED;
    xr_random_bytes(arguments->nodes[0].as.u8_array.data, arguments->nodes[0].as.u8_array.size);
    result->count = 1u;
    result->nodes[0] = (XrProviderValueNode){.token = XR_PROVIDER_TYPE_UNIT};
    return XR_PROVIDER_CALL_OK;
}
#endif
