/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtext_worker.c - Local RC-owned compiler output storage
 */
#include "xtext_worker.h"
#include "xstring.h"
#include "../xstrbuf.h"
#include "../core/xr_exec_context.h"
#include "../core/xr_runtime_core.h"
#include "../mem/xcoro_heap.h"
#include "../xisolate_api.h"

typedef struct XrTextWorker {
    XrObjHeader header;
    XrCoroHeap *owner;
    XrStrBuf *buffer;
    uint8_t role, state;
} XrTextWorker;
enum { TEXT_WORKER_OPEN = 1, TEXT_WORKER_CLOSED = 2 };

static bool worker_role_valid(XrTextWorkerRole role) {
    return role == XR_TEXT_WORKER_CONCAT || role == XR_TEXT_WORKER_PRINT;
}
static XrCoroHeap *worker_current_heap(XrVMRuntime *vm) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(vm);
    XrAllocationContext *allocation = xr_alloc_context_current();
    return core && xr_runtime_core_vm_owner(core) == vm &&
           allocation && allocation->core == core &&
           allocation->domain == XR_STORAGE_EXEC_LOCAL && allocation->local_heap &&
           allocation->local_heap->core == core ? allocation->local_heap : NULL;
}
bool xr_text_worker_is_private(XrValue value) {
    return XR_IS_PTR(value) &&
           (value.heap_type == XR_TTEXT_WORKER ||
            (value.heap_type != XR_TSTRING && value.ptr &&
             ((XrObjHeader *) value.ptr)->type == XR_TTEXT_WORKER));
}
static XrTextWorker *worker_check(XrVMRuntime *vm, XrValue value,
                                   XrTextWorkerRole role, bool allow_closed) {
    XrCoroHeap *heap = worker_current_heap(vm);
    if (!heap || !worker_role_valid(role) || !XR_IS_PTR(value) ||
        value.heap_type != XR_TTEXT_WORKER || !value.ptr)
        return NULL;
    XrObjHeader *header = (XrObjHeader *) value.ptr;
    if (header->type != XR_TTEXT_WORKER ||
        header->objsize != XR_HEAP_ALIGN(sizeof(XrTextWorker)) ||
        XR_OBJ_GET_STORAGE(header) != XR_OBJ_STORAGE_NORMAL ||
        !XR_OBJ_HAS_DESTRUCTOR(header) ||
        (header->extra & (XR_OBJ_DEAD | XR_OBJ_MANAGED | XR_OBJ_ATOMIC | XR_OBJ_IMMORTAL |
                          XR_OBJ_AOT_NATIVE | XR_OBJ_TRANSIT)) != 0)
        return NULL;
    XrTextWorker *worker = (XrTextWorker *) header;
    if (worker->owner != heap || worker->role != role ||
        (worker->state != TEXT_WORKER_OPEN &&
         !(allow_closed && worker->state == TEXT_WORKER_CLOSED)))
        return NULL;
    if (worker->state == TEXT_WORKER_CLOSED)
        return worker->buffer == NULL ? worker : NULL;
    if (!worker->buffer || worker->buffer->X != vm || !worker->buffer->data ||
        worker->buffer->length > worker->buffer->capacity)
        return NULL;
    return worker;
}
void xr_obj_destroy_text_worker(XrObjHeader *object, XrCoroHeap *heap) {
    if (!object || object->type != XR_TTEXT_WORKER ||
        object->objsize != XR_HEAP_ALIGN(sizeof(XrTextWorker)))
        return;
    XrTextWorker *worker = (XrTextWorker *) object;
    if (!heap || !heap->core || worker->owner != heap ||
        !worker_role_valid((XrTextWorkerRole) worker->role) ||
        (worker->state != TEXT_WORKER_OPEN && worker->state != TEXT_WORKER_CLOSED))
        return;
    XrStrBuf *buffer = worker->buffer;
    if ((worker->state == TEXT_WORKER_CLOSED && buffer) ||
        (buffer && buffer->X != xr_runtime_core_vm_owner(heap->core)))
        return;
    worker->buffer = NULL;
    worker->state = TEXT_WORKER_CLOSED;
    xr_strbuf_free(buffer);
}
XrTextWorkerStatus xr_text_worker_new(XrVMRuntime *vm, XrTextWorkerRole role, XrValue *out) {
    XrCoroHeap *heap = worker_current_heap(vm);
    if (!heap || !out || !XR_IS_NULL(*out) || !worker_role_valid(role) ||
        xr_runtime_core_destroy_op(heap->core, XR_TTEXT_WORKER) != xr_obj_destroy_text_worker)
        return XR_TEXT_WORKER_INVALID;
    XrTextWorker *worker = (XrTextWorker *)
        xr_coro_heap_new_obj(heap, XR_TTEXT_WORKER, sizeof(*worker));
    if (!worker) return XR_TEXT_WORKER_OUT_OF_MEMORY;
    worker->owner = heap;
    worker->buffer = NULL;
    worker->role = (uint8_t) role;
    worker->state = TEXT_WORKER_CLOSED;
    worker->buffer = xr_strbuf_new(vm, 64u);
    if (!worker->buffer) {
        xr_coro_heap_destroy_obj(heap, &worker->header);
        return XR_TEXT_WORKER_OUT_OF_MEMORY;
    }
    worker->state = TEXT_WORKER_OPEN;
    *out = xr_make_typed_ptr_val(worker, XR_TTEXT_WORKER);
    return XR_TEXT_WORKER_OK;
}
XrTextWorkerStatus xr_text_worker_append_bytes(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, const char *bytes, size_t length) {
    XrTextWorker *worker = worker_check(vm, value, role, false);
    if (!worker || (!bytes && length)) return XR_TEXT_WORKER_INVALID;
    return xr_strbuf_append_cstr(worker->buffer, bytes, length)
               ? XR_TEXT_WORKER_OK : XR_TEXT_WORKER_OUT_OF_MEMORY;
}
XrTextWorkerStatus xr_text_worker_append_int(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, int64_t number) {
    XrTextWorker *worker = worker_check(vm, value, role, false);
    if (!worker) return XR_TEXT_WORKER_INVALID;
    return xr_strbuf_append_int(worker->buffer, number)
               ? XR_TEXT_WORKER_OK : XR_TEXT_WORKER_OUT_OF_MEMORY;
}
XrTextWorkerStatus xr_text_worker_append_float(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, double number) {
    XrTextWorker *worker = worker_check(vm, value, role, false);
    if (!worker) return XR_TEXT_WORKER_INVALID;
    return xr_strbuf_append_float(worker->buffer, number)
               ? XR_TEXT_WORKER_OK : XR_TEXT_WORKER_OUT_OF_MEMORY;
}
XrTextWorkerStatus xr_text_worker_view(XrVMRuntime *vm, XrValue value,
    XrTextWorkerRole role, XrTextWorkerView *out) {
    XrTextWorker *worker = worker_check(vm, value, role, false);
    if (!worker || !out) return XR_TEXT_WORKER_INVALID;
    *out = (XrTextWorkerView) {worker->buffer->data, worker->buffer->length};
    return XR_TEXT_WORKER_OK;
}
XrTextWorkerStatus xr_text_worker_close(XrVMRuntime *vm, XrValue *value,
                                        XrTextWorkerRole role) {
    if (!value) return XR_TEXT_WORKER_INVALID;
    if (XR_IS_NULL(*value)) return XR_TEXT_WORKER_OK;
    XrTextWorker *worker = worker_check(vm, *value, role, true);
    if (!worker) return XR_TEXT_WORKER_INVALID;
    XrCoroHeap *heap = worker->owner;
    *value = xr_null();
    xr_coro_heap_destroy_obj(heap, &worker->header);
    return XR_TEXT_WORKER_OK;
}
XrTextWorkerStatus xr_text_worker_finish(XrVMRuntime *vm, XrValue *value) {
    if (!value) return XR_TEXT_WORKER_INVALID;
    XrTextWorker *worker = worker_check(vm, *value, XR_TEXT_WORKER_CONCAT, false);
    if (!worker) return XR_TEXT_WORKER_INVALID;
    XrString *string = xr_strbuf_to_string(worker->buffer);
    XrTextWorkerStatus closed = xr_text_worker_close(vm, value, XR_TEXT_WORKER_CONCAT);
    if (closed != XR_TEXT_WORKER_OK) return closed;
    if (!string) return XR_TEXT_WORKER_OUT_OF_MEMORY;
    *value = xr_string_value(string);
    return XR_TEXT_WORKER_OK;
}
