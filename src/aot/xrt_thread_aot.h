/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_thread_aot.h - AOT trampoline for sys.Thread.spawn.
 */

#ifndef XRT_THREAD_AOT_H
#define XRT_THREAD_AOT_H

#include "xrt_sys.h"

#ifdef XRT_ENABLE_SYS_THREAD
#include "xrt_exception.h"
#include "xaot_coro.h"

typedef struct xrt_thread_aot_entry {
    xrt_thread_object_t *thread;
    const XrAotCoroDesc *desc;
    void *frame;
} xrt_thread_aot_entry_t;

static inline void xrt_thread_aot_release_frame(const XrAotCoroDesc *desc, void *frame) {
    if (!frame)
        return;
    if (desc && desc->release_frame)
        desc->release_frame(frame, NULL);
    else
        xr_aot_frame_free(frame);
}

static inline void xrt_thread_aot_set_pending_error(const char *message) {
    if (!message)
        return;
    XRT_THREAD_SET_PENDING_ERROR(xrt_exception_new_value(XR_ERR_RUNTIME, message, strlen(message)));
}

static void *xrt_thread_aot_entry(void *arg) {
    xrt_thread_aot_entry_t *entry = (xrt_thread_aot_entry_t *) arg;
    if (!entry)
        return NULL;

    xrt_thread_object_t *thread = entry->thread;
    const XrAotCoroDesc *desc = entry->desc;
    void *frame = entry->frame;
    XRT_FREE(entry);

    if (!thread)
        return NULL;

    if (desc && desc->name)
        xr_thread_set_name(xr_thread_self(), desc->name);

    XrAotResult result =
        desc && desc->resume ? desc->resume(frame, NULL) : xr_aot_error(XR_NULL_VAL, false);
    if (result.kind == XR_AOT_RUN_DONE) {
        thread->retval = result.value;
        thread->error = XR_NULL_VAL;
        thread->error_is_value = false;
        atomic_store_explicit(&thread->failed, false, memory_order_release);
    } else {
        thread->retval = XR_NULL_VAL;
        thread->error = result.error;
        thread->error_is_value = result.error_is_value;
        atomic_store_explicit(&thread->failed, true, memory_order_release);
    }
    xrt_thread_aot_release_frame(desc, frame);
    atomic_store_explicit(&thread->finished, true, memory_order_release);

    xrt_release(xrt_thread_box(thread));
    return NULL;
}

static inline XrValue xrt_thread_spawn_aot(const XrAotCoroDesc *desc, void *frame,
                                           size_t stack_size) {
    if (!desc || !desc->resume || !frame) {
        xrt_thread_aot_release_frame(desc, frame);
        xrt_thread_aot_set_pending_error("sys.Thread.spawn: failed to allocate thread frame");
        return XR_NULL_VAL;
    }

    xrt_thread_object_t *thread = (xrt_thread_object_t *) xrt_arc_alloc(sizeof(*thread));
    xrt_arc_mark_builtin(thread, XRT_ARC_KIND_THREAD);
    thread->handle = (xr_thread_t) {0};
    atomic_store_explicit(&thread->state, XRT_THREAD_CREATED, memory_order_relaxed);
    atomic_store_explicit(&thread->finished, false, memory_order_relaxed);
    atomic_store_explicit(&thread->failed, false, memory_order_relaxed);
    thread->error_is_value = false;
    thread->retval = XR_NULL_VAL;
    thread->error = XR_NULL_VAL;

    xrt_thread_aot_entry_t *entry = (xrt_thread_aot_entry_t *) XRT_MALLOC(sizeof(*entry));
    if (!entry) {
        xrt_thread_aot_release_frame(desc, frame);
        xrt_thread_aot_set_pending_error("sys.Thread.spawn: unable to allocate thread entry");
        xrt_release(xrt_thread_box(thread));
        return XR_NULL_VAL;
    }
    entry->thread = thread;
    entry->desc = desc;
    entry->frame = frame;

    XrValue handle = xrt_thread_box(thread);
    xrt_retain(handle);
    if (!xr_thread_create_ex(&thread->handle, xrt_thread_aot_entry, entry, stack_size)) {
        XRT_FREE(entry);
        xrt_thread_aot_release_frame(desc, frame);
        xrt_release(handle);
        xrt_release(handle);
        xrt_thread_aot_set_pending_error("sys.Thread.spawn: OS thread creation failed");
        return XR_NULL_VAL;
    }
    if (desc->name)
        xr_thread_set_name(thread->handle, desc->name);
    return handle;
}
#endif

#endif /* XRT_THREAD_AOT_H */
