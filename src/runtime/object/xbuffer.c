/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuffer.c - Managed raw-byte storage runtime object
 *
 * KEY CONCEPT:
 *   The runtime owns Buffer's native storage, deterministic destruction and
 *   compiler materialization bridge. Allocation choices and range validation
 *   belong to stdlib/mem/mem.xr.
 */

#include "xbuffer.h"
#include "../class/xclass.h"
#include "../class/xclass_system.h"
#include "../class/xinstance.h"
#include "../core/xr_exec_context.h"
#include "../core/xr_runtime_core.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xcoro_heap.h"
#include "../value/xstruct_layout.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../shared/xr_elem_type.h"

#include <stdint.h>
#include <string.h>

typedef struct XrBufferBody {
    void *data;
    int64_t length;
    size_t align;
    XrSliceView readonly_span_cache;
    XrSliceView mutable_span_cache;
} XrBufferBody;

static void xr_buffer_body_destroy(void *body) {
    XrBufferBody *buffer = (XrBufferBody *) body;
    if (!buffer)
        return;
    if (buffer->align > 0)
        xr_free_aligned(buffer->data, buffer->align);
    else
        xr_free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static XrNativeBodyDesc g_buffer_body_desc = {
    .body_size = sizeof(XrBufferBody),
    .body_align = 0,
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = xr_buffer_body_destroy,
    .deep_copy = NULL,
};

XrNativeBodyDesc *xr_buffer_native_body_desc(void) {
    return &g_buffer_body_desc;
}

static XrBufferBody *xr_buffer_builtin_body(XrValue value) {
    if (!XR_IS_INSTANCE(value))
        return NULL;
    XrObjectInstance *instance = (XrObjectInstance *) XR_TO_PTR(value);
    if (!instance || !instance->klass || instance->klass->builtin_kind != XR_BK_BUFFER)
        return NULL;
    return (XrBufferBody *) xr_instance_native_body(instance);
}

static XrBufferBody *xr_buffer_instance_body(XrVMRuntime *isolate, XrValue value) {
    if (!isolate || !XR_IS_INSTANCE(value))
        return NULL;
    XrObjectInstance *instance = (XrObjectInstance *) XR_TO_PTR(value);
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XrClass *buffer_class = core ? core->memBufferClass : NULL;
    if (!instance || !buffer_class || !xr_class_instanceof(instance->klass, buffer_class))
        return NULL;
    return (XrBufferBody *) xr_instance_native_body(instance);
}

static void *xr_buffer_allocate_bytes(size_t size, bool zeroed, size_t align) {
    if (align == 0)
        return zeroed ? xr_calloc(1, size) : xr_malloc(size);
    if (align < sizeof(void *) || (align & (align - 1)) != 0)
        return NULL;
    void *data = xr_malloc_aligned(size, align);
    if (data && zeroed)
        memset(data, 0, size);
    return data;
}

static void xr_buffer_release_bytes(XrBufferBody *buffer) {
    if (!buffer || !buffer->data)
        return;
    if (buffer->align > 0)
        xr_free_aligned(buffer->data, buffer->align);
    else
        xr_free(buffer->data);
    buffer->data = NULL;
}

XrValue xr_buffer_new(XrVMRuntime *isolate, int64_t length, bool zeroed, size_t align) {
    if (!isolate || length < 0 || (uint64_t) length > SIZE_MAX)
        return XR_NULL_VAL;
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XrClass *buffer_class = core ? core->memBufferClass : NULL;
    XR_DCHECK(buffer_class != NULL, "mem.Buffer class not registered");
    if (!buffer_class)
        return XR_NULL_VAL;

    XrObjectInstance *instance = xr_instance_new(isolate, buffer_class);
    XR_CHECK(instance != NULL, "mem.Buffer allocation failed");
    XrBufferBody *buffer = (XrBufferBody *) xr_instance_native_body(instance);
    XR_CHECK(buffer != NULL, "mem.Buffer native body missing");
    buffer->align = align;
    buffer->length = length;
    if (length > 0) {
        buffer->data = xr_buffer_allocate_bytes((size_t) length, zeroed, align);
        XR_CHECK(buffer->data != NULL, "mem.Buffer byte allocation failed");
    }
    return XR_FROM_PTR(instance);
}

int64_t xr_buffer_length(XrValue value) {
    XrBufferBody *buffer = xr_buffer_builtin_body(value);
    return buffer ? buffer->length : -1;
}

void *xr_buffer_borrow_pointer(XrVMRuntime *isolate, XrValue value) {
    XrBufferBody *buffer = xr_buffer_instance_body(isolate, value);
    return buffer ? buffer->data : NULL;
}

XrValue xr_buffer_byte_view(XrVMRuntime *isolate, XrValue value, bool readonly) {
    XrBufferBody *buffer = xr_buffer_instance_body(isolate, value);
    if (!buffer)
        return xr_span_ref_typed(NULL, XR_ELEM_U8, 1, 0, 0, readonly ? XR_SLICE_VIEW_READONLY : 0);
    XrSliceView *span = readonly ? &buffer->readonly_span_cache : &buffer->mutable_span_cache;
    span->data = buffer->data;
    span->length = buffer->length;
    return xr_span_ref_typed(span, XR_ELEM_U8, 1, 0, 0, readonly ? XR_SLICE_VIEW_READONLY : 0);
}

bool xr_buffer_resize(XrVMRuntime *isolate, XrValue value, int64_t new_length) {
    XrBufferBody *buffer = xr_buffer_instance_body(isolate, value);
    if (!buffer || new_length < 0 || (uint64_t) new_length > SIZE_MAX)
        return false;
    if (new_length == 0) {
        xr_buffer_release_bytes(buffer);
        buffer->length = 0;
        return true;
    }

    void *new_data = NULL;
    if (buffer->align > 0) {
        new_data = xr_buffer_allocate_bytes((size_t) new_length, false, buffer->align);
        if (new_data && buffer->data) {
            size_t old_length = buffer->length > 0 ? (size_t) buffer->length : 0;
            size_t copy_length =
                old_length < (size_t) new_length ? old_length : (size_t) new_length;
            memcpy(new_data, buffer->data, copy_length);
        }
    } else {
        new_data = xr_realloc(buffer->data, (size_t) new_length);
    }
    if (!new_data)
        return false;
    if (buffer->align > 0)
        xr_buffer_release_bytes(buffer);
    buffer->data = new_data;
    buffer->length = new_length;
    return true;
}

bool xr_buffer_bytes(XrValue value, const uint8_t **data, size_t *length) {
    if (!data || !length)
        return false;
    XrBufferBody *buffer = xr_buffer_builtin_body(value);
    if (!buffer || buffer->length < 0 || (buffer->length > 0 && !buffer->data))
        return false;
    *data = (const uint8_t *) buffer->data;
    *length = (size_t) buffer->length;
    return true;
}

static bool xr_buffer_copy_initialized_layout(uint8_t *dst, const uint8_t *src,
                                              const XrAggregateLayout *layout, unsigned depth) {
    if (!dst || !src || !layout || depth > 16)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible || field->offset > layout->total_size ||
            field->size > layout->total_size - field->offset)
            return false;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!field->sub_layout || field->sub_layout->total_size != field->size ||
                !xr_buffer_copy_initialized_layout(dst + field->offset, src + field->offset,
                                                   field->sub_layout, depth + 1))
                return false;
        } else {
            memcpy(dst + field->offset, src + field->offset, field->size);
        }
    }
    return true;
}

bool xr_buffer_materialize(XrValue value, void *dst, size_t size, size_t align,
                           const XrAggregateLayout *layout) {
    if (!dst || size == 0 || align == 0)
        return false;
    XrBufferBody *buffer = xr_buffer_builtin_body(value);
    if (!buffer || buffer->length != (int64_t) size || buffer->align != align || !buffer->data ||
        ((uintptr_t) buffer->data % align) != 0)
        return false;

    memset(dst, 0, size);
    bool copied = layout ? (layout->total_size == size &&
                            xr_buffer_copy_initialized_layout(
                                (uint8_t *) dst, (const uint8_t *) buffer->data, layout, 0))
                         : (memcpy(dst, buffer->data, size), true);
    if (!copied)
        return false;
    xr_buffer_release_bytes(buffer);
    buffer->length = 0;
    buffer->align = 0;
    return true;
}

XrValue xr_buffer_copy_from_bytes(XrVMRuntime *isolate, const uint8_t *data, size_t length) {
    if (!isolate || length > (size_t) INT64_MAX || (length > 0 && !data))
        return XR_NULL_VAL;

    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    XrAllocationContext transfer_alloc;
    XrExecutionContext transfer_exec;
    xr_alloc_context_init(&transfer_alloc, core, XR_STORAGE_TRANSFERABLE);
    xr_exec_context_init(&transfer_exec, core, &transfer_alloc);
    XrExecutionContext *previous = xr_exec_context_enter(&transfer_exec);
    XrValue value = xr_buffer_new(isolate, (int64_t) length, false, 0);
    xr_exec_context_restore(previous);

    XrBufferBody *buffer = xr_buffer_instance_body(isolate, value);
    if (!buffer)
        return XR_NULL_VAL;
    if (length > 0)
        memcpy(buffer->data, data, length);
    return value;
}
