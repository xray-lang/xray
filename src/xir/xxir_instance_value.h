/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_value.h - Synchronous owned-root access through the current instance
 *
 * KEY CONCEPT:
 *   A temporary receiver binds an owning root; mutation resolves its current
 *   contents only after argument evaluation and never exports an element address.
 */
#ifndef XXIR_INSTANCE_VALUE_H
#define XXIR_INSTANCE_VALUE_H
#include "xxir_program.h"
#include "xxir_array.h"

typedef enum XrXirValueRootKind {
    XR_XIR_ROOT_VALUE, XR_XIR_ROOT_LOCAL, XR_XIR_ROOT_CELL, XR_XIR_ROOT_SLOT,
    XR_XIR_ROOT_OBJECT /* a class identity whose fields start the path */
} XrXirValueRootKind;
typedef struct XrXirValueReceiver {
    XrXirValueRootKind kind;
    XrXirType type;
    XrXirValue value;
    void *local_payload;
    uint32_t slot;
} XrXirValueReceiver;

XR_FUNC XrXirCallStatus xr_xir_instance_path_read(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_path_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_path_push(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_path_length(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault);

XR_FUNC XrXirCallStatus xr_xir_instance_array_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_array_read(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, bool length,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_array_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, const XrXirValue *element,
    bool append, XrXirFaultDetail *fault);
XR_FUNC XrXirCallStatus xr_xir_instance_struct_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, uint32_t field, const XrXirValue *value);
#endif // XXIR_INSTANCE_VALUE_H
