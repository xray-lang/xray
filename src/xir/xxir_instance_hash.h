/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_hash.h - Active-call hash operations on authenticated value roots
 */
#ifndef XXIR_INSTANCE_HASH_H
#define XXIR_INSTANCE_HASH_H
#include "xxir_instance_value.h"
#include "xxir_map.h"
#include "xxir_nullable.h"
typedef struct XrXirHashRequest {
    XrXirOp operation;
    XrXirType result;
    uint32_t instruction;
    const XrXirValueReceiver *receiver;
    const XrXirValuePath *path;
    const XrXirValue *key, *value;
} XrXirHashRequest;
/* Construction consumes interleaved Map key/value pairs or Set elements. */
XR_FUNC XrXirCallStatus xr_xir_instance_hash_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output);
/* Root/path selection precedes arguments; contents are read at this operation.
 * A failed private mutation or final path write preserves the original root. */
XR_FUNC XrXirCallStatus xr_xir_instance_hash_apply(XrXirCallView *view,
    const XrXirHashRequest *request, XrXirValue *output, XrXirFaultDetail *fault);
#endif // XXIR_INSTANCE_HASH_H
