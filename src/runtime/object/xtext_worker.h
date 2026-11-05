/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtext_worker.h - Execution-private compiler text accumulation
 */
#ifndef XTEXT_WORKER_H
#define XTEXT_WORKER_H
#include "../value/xvalue.h"
#include <stddef.h>

struct XrCoroHeap;
typedef enum XrTextWorkerRole {
    XR_TEXT_WORKER_CONCAT = 1, XR_TEXT_WORKER_PRINT = 2
} XrTextWorkerRole;
typedef enum XrTextWorkerStatus {
    XR_TEXT_WORKER_OK = 0, XR_TEXT_WORKER_INVALID, XR_TEXT_WORKER_OUT_OF_MEMORY
} XrTextWorkerStatus;
typedef struct XrTextWorkerView { const char *bytes; size_t length; } XrTextWorkerView;
XR_FUNC bool xr_text_worker_is_private(XrValue value);
XR_FUNC XrTextWorkerStatus xr_text_worker_new(XrVMRuntime *vm, XrTextWorkerRole role, XrValue *out);
XR_FUNC XrTextWorkerStatus xr_text_worker_append_bytes(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, const char *bytes, size_t length);
XR_FUNC XrTextWorkerStatus xr_text_worker_append_int(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, int64_t number);
XR_FUNC XrTextWorkerStatus xr_text_worker_append_float(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, double number);
XR_FUNC XrTextWorkerStatus xr_text_worker_view(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, XrTextWorkerView *out);
XR_FUNC XrTextWorkerStatus xr_text_worker_close(XrVMRuntime *vm, XrValue *value,
    XrTextWorkerRole role);
XR_FUNC XrTextWorkerStatus xr_text_worker_finish(XrVMRuntime *vm, XrValue *value);
XR_FUNC void xr_obj_destroy_text_worker(XrObjHeader *object, struct XrCoroHeap *heap);
#endif // XTEXT_WORKER_H
