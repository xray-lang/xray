/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * text_worker_fault_cases.c - Real worker, buffer and heap cleanup under failed boundaries
 */
#if defined(_MSC_VER)
/* MSVC diagnoses standard C11 flexible array members as an extension. */
#pragma warning(push)
#pragma warning(disable: 4200)
#endif
#include "base/xmalloc.h"
#include "runtime/object/xtext_worker.h"
#include "runtime/object/xstring.h"
#include "runtime/xstrbuf.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/mem/xcoro_heap.h"
#include "runtime/xisolate_internal.h"
#include "xray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "text worker requirement failed at %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)
static size_t allocation_calls, fail_at, live_allocations;
static bool reject_growth, reject_header, reject_intern;
static void *fault_malloc(size_t bytes) {
    if (++allocation_calls == fail_at) return NULL;
    void *pointer = malloc(bytes);
    if (pointer) ++live_allocations;
    return pointer;
}
static void fault_free(void *pointer) {
    if (pointer) { REQUIRE(live_allocations != 0u); --live_allocations; }
    free(pointer);
}
static void *fault_realloc(void *pointer, size_t bytes) {
    return reject_growth ? NULL : realloc(pointer, bytes);
}
static XrObjHeader *fault_header(XrCoroHeap *heap, uint8_t type, size_t size) {
    return reject_header ? NULL : xr_coro_heap_new_obj(heap, type, size);
}
static XrString *fault_intern(XrStrBuf *buffer) {
    return reject_intern ? NULL : xr_strbuf_to_string(buffer);
}
/* Include the single production buffer and worker bodies. Only allocation and
 * final interning are intercepted; object registration, owner validation, RC,
 * explicit close, and heap teardown execute their real linked implementations. */
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) fault_malloc(size)
#define xr_free(pointer) fault_free(pointer)
#define xr_realloc(pointer, size) fault_realloc(pointer, size)
#define xr_strbuf_new probe_strbuf_new
#define xr_strbuf_free probe_strbuf_free
#define xr_strbuf_ensure probe_strbuf_ensure
#define xr_strbuf_reserve probe_strbuf_reserve
#define xr_strbuf_append_cstr probe_strbuf_append_cstr
#define xr_strbuf_append_str probe_strbuf_append_str
#define xr_strbuf_append_char probe_strbuf_append_char
#define xr_strbuf_append_int probe_strbuf_append_int
#define xr_strbuf_append_float probe_strbuf_append_float
#define xr_strbuf_reset probe_strbuf_reset
#include "../../../src/runtime/xstrbuf.c"
#define xr_coro_heap_new_obj fault_header
#define xr_strbuf_to_string fault_intern
#define xr_text_worker_is_private probe_worker_is_private
#define xr_text_worker_new probe_worker_new
#define xr_text_worker_append_bytes probe_worker_append_bytes
#define xr_text_worker_append_int probe_worker_append_int
#define xr_text_worker_append_float probe_worker_append_float
#define xr_text_worker_view probe_worker_view
#define xr_text_worker_close probe_worker_close
#define xr_text_worker_finish probe_worker_finish
#define xr_obj_destroy_text_worker probe_destroy_worker
#include "../../../src/runtime/object/xtext_worker.c"

int text_worker_fault_cases(void) {
    for (unsigned mode = 0; mode < 9u; ++mode) {
        XrVMConfig config = {0};
        XrVMRuntime *vm = xray_vm_new_full(&config);
        REQUIRE(vm != NULL);
        xr_runtime_core_set_destroy_op(vm->core_rt, XR_TTEXT_WORKER, probe_destroy_worker);
        xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
        XrCoroHeap *heap = &vm->core_rt->root_heap;
        XrCoroHeap *local = NULL;
        XrAllocationContext allocation;
        XrExecutionContext execution;
        if (mode == 7u) {
            local = xr_coro_heap_create(vm->core_rt);
            REQUIRE(local != NULL);
            xr_alloc_context_init(&allocation, vm->core_rt, XR_STORAGE_EXEC_LOCAL);
            allocation.local_heap = local;
            xr_exec_context_init(&execution, vm->core_rt, &allocation);
            xr_exec_context_enter(&execution);
            heap = local;
        }
        allocation_calls = 0u;
        fail_at = mode == 1u ? 1u : mode == 2u ? 2u : 0u;
        reject_header = mode == 5u;
        reject_growth = mode == 3u;
        reject_intern = mode == 4u;
        uint32_t objects = heap->object_count;
        XrValue value = xr_null();
        XrTextWorkerStatus status = probe_worker_new(vm, XR_TEXT_WORKER_CONCAT, &value);
        if (mode == 1u || mode == 2u || mode == 5u) {
            REQUIRE(status == XR_TEXT_WORKER_OUT_OF_MEMORY && XR_IS_NULL(value));
            REQUIRE(heap->object_count == objects && live_allocations == 0u);
        } else {
            REQUIRE(status == XR_TEXT_WORKER_OK && live_allocations == 2u);
            XrTextWorker *body = (XrTextWorker *) value.ptr;
            XrTextWorkerView invalid_view = {0};
            uint8_t state = body->state;
            body->state = 0;
            REQUIRE(probe_worker_view(vm, value, XR_TEXT_WORKER_CONCAT, &invalid_view) == XR_TEXT_WORKER_INVALID);
            body->state = state;
            XrCoroHeap *owner = body->owner;
            body->owner = NULL;
            REQUIRE(probe_worker_view(vm, value, XR_TEXT_WORKER_CONCAT, &invalid_view) == XR_TEXT_WORKER_INVALID);
            body->owner = owner;
            char payload[512]; memset(payload, 'q', sizeof(payload));
            status = probe_worker_append_bytes(vm, value, XR_TEXT_WORKER_CONCAT,
                                                payload, sizeof(payload));
            if (mode == 3u) {
                REQUIRE(status == XR_TEXT_WORKER_OUT_OF_MEMORY);
                XrTextWorkerView view = {0};
                REQUIRE(probe_worker_view(vm, value, XR_TEXT_WORKER_CONCAT, &view) == XR_TEXT_WORKER_OK);
                REQUIRE(view.length == 0u);
                reject_growth = false;
                REQUIRE(probe_worker_append_bytes(vm, value, XR_TEXT_WORKER_CONCAT, payload, 64u) == XR_TEXT_WORKER_OK);
                reject_growth = true;
                REQUIRE(probe_worker_append_int(vm, value, XR_TEXT_WORKER_CONCAT, INT64_MIN) == XR_TEXT_WORKER_OUT_OF_MEMORY);
                REQUIRE(probe_worker_append_float(vm, value, XR_TEXT_WORKER_CONCAT, 1.5) == XR_TEXT_WORKER_OUT_OF_MEMORY);
                REQUIRE(probe_worker_close(vm, &value, XR_TEXT_WORKER_CONCAT) == XR_TEXT_WORKER_OK);
            } else {
                REQUIRE(status == XR_TEXT_WORKER_OK);
                if (mode == 4u) {
                    REQUIRE(probe_worker_finish(vm, &value) == XR_TEXT_WORKER_OUT_OF_MEMORY);
                    REQUIRE(XR_IS_NULL(value));
                } else if (mode == 8u) {
                    xr_rc_release_value(heap, value);
                    value = xr_null();
                } else if (mode != 6u && mode != 7u) {
                    REQUIRE(probe_worker_finish(vm, &value) == XR_TEXT_WORKER_OK);
                    REQUIRE(XR_IS_STRING(value) && XR_TO_STRING(value)->length == sizeof(payload));
                }
            }
            if (mode != 6u && mode != 7u)
                REQUIRE(live_allocations == 0u && heap->object_count == objects);
        }
        /* Abandonment uses genuine registered finalization, not a synthetic
         * close. VM cancellation/unwind trigger integration remains a separate gate. */
        if (local) {
            xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
            xr_coro_heap_destroy(local);
            REQUIRE(live_allocations == 0u);
        }
        xray_vm_delete(vm);
        REQUIRE(live_allocations == 0u);
    }
    return 0;
}
