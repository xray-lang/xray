/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_lower_control.h - Synchronous bounded copy for private lowering metadata
 *
 * KEY CONCEPT: A caller owns both spans and the borrowed request for this call.
 */
#ifndef XXIR_LOWER_CONTROL_H
#define XXIR_LOWER_CONTROL_H
#include "../base/xcompile_request.h"
#include "../base/xsha256.h"
#include <string.h>

static inline XrCompileResourceStatus xir_lower_copy_span(XrCompileRequestControl *control,
    void *destination,const void *source,size_t bytes) {
    if (bytes && (!destination || !source)) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (!control) { if (bytes) memcpy(destination,source,bytes);return XR_COMPILE_RESOURCE_OK; }
    for (size_t offset=0;offset<bytes;) {
        XrCompileResourceStatus status=xr_compile_request_control_poll(control);
        if (status!=XR_COMPILE_RESOURCE_OK) return status;
        size_t count=bytes-offset;
        if (count>4096) count=4096;
        memcpy((unsigned char *)destination+offset,(const unsigned char *)source+offset,count);
        offset+=count;
    }
    return xr_compile_request_control_poll(control);
}
static inline XrCompileResourceStatus xir_lower_sha256_span(XrCompileRequestControl *control,
    XrSHA256Context *hash,const void *source,size_t bytes) {
    if (!hash || (bytes && !source)) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (!control) { xr_sha256_update(hash,source,bytes);return XR_COMPILE_RESOURCE_OK; }
    for (size_t offset=0;offset<bytes;) {
        XrCompileResourceStatus status=xr_compile_request_control_poll(control);
        if (status!=XR_COMPILE_RESOURCE_OK) return status;
        size_t count=bytes-offset;
        if (count>4096) count=4096;
        xr_sha256_update(hash,(const unsigned char *)source+offset,count);
        offset+=count;
    }
    return xr_compile_request_control_poll(control);
}
#endif // XXIR_LOWER_CONTROL_H
