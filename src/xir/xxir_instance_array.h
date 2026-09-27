/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_array.h - Synchronous Array access through the current instance
 *
 * KEY CONCEPT:
 *   A temporary receiver binds an owning root; mutation resolves its current
 *   contents only after argument evaluation and never exports an element address.
 */
#ifndef XXIR_INSTANCE_ARRAY_H
#define XXIR_INSTANCE_ARRAY_H
#include "xxir_program.h"
#include "xxir_array.h"

typedef enum XrXirArrayRootKind {
    XR_XIR_ARRAY_VALUE, XR_XIR_ARRAY_LOCAL, XR_XIR_ARRAY_CELL, XR_XIR_ARRAY_SLOT
} XrXirArrayRootKind;
typedef struct XrXirArrayReceiver {
    XrXirArrayRootKind kind;
    XrXirType type;
    XrXirValue value;
    void *local_payload;
    uint32_t slot;
} XrXirArrayReceiver;

XR_FUNC XrXirCallStatus xr_xir_instance_array_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_array_read(XrXirCallView *view,
    const XrXirArrayReceiver *receiver, int64_t index, bool length,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_array_write(XrXirCallView *view,
    const XrXirArrayReceiver *receiver, int64_t index, const XrXirValue *element,
    bool append, XrXirFaultDetail *fault);
#endif // XXIR_INSTANCE_ARRAY_H
