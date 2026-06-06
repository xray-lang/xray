/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock.c - Backend-neutral coroutine blocking helpers
 */

#include "xblock.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xarray.h"
#include "../runtime/xisolate_internal.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xdeep_copy.h"
#include "xtask.h"
#include "xworker.h"
#include "xyieldable.h"

static inline XrCoroBlockResult block_result(XrCoroBlockKind kind, XrValue value, bool ok) {
    XrCoroBlockResult result = {kind, value, ok};
    return result;
}

XrValue *xr_slot_value_address(XrSlotRef slot) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return NULL;
        case XR_SLOT_XVALUE_PTR:
            return (XrValue *) slot.base;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND:
            if (slot.type_id != XR_REP_TAGGED)
                return NULL;
            if (!slot.base)
                return NULL;
            return (XrValue *) ((uint8_t *) slot.base + slot.offset);
        default:
            return NULL;
    }
}

bool xr_slot_store_value(XrSlotRef slot, XrValue value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = value;
            return true;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = XR_TO_INT(value);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *(double *) addr = XR_TO_FLOAT(value);
                return true;
            }
            *(XrValue *) addr = value;
            return true;
        }
        default:
            return false;
    }
}

bool xr_slot_load_value(XrSlotRef slot, XrValue *out_value) {
    if (!out_value)
        return false;
    switch (slot.kind) {
        case XR_SLOT_NONE:
            *out_value = xr_null();
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *out_value = *(XrValue *) slot.base;
            return true;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *out_value = xr_int(*(int64_t *) addr);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *out_value = xr_float(*(double *) addr);
                return true;
            }
            *out_value = *(XrValue *) addr;
            return true;
        }
        default:
            return false;
    }
}

bool xr_coro_store_recv_value(XrCoroutine *coro, XrValue value) {
    if (!coro)
        return false;
    XrCoroExt *ext = coro->ext;
    if (!ext)
        return false;
    if (ext->recv_slot_ref.kind != XR_SLOT_NONE)
        return xr_slot_store_value(ext->recv_slot_ref, value);
    if (ext->recv_slot) {
        *ext->recv_slot = value;
        return true;
    }
    return false;
}

static void coro_finish_resume(XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_finish_resume: NULL coro");
    xr_coro_resume_store(coro, XR_RESUME_OK);
    if (coro->ext) {
        xr_channel_wait_token_finish(&coro->ext->chan_wait_token);
        xr_timer_wait_token_finish(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->wait_channel, NULL, memory_order_relaxed);
    }
}

static void coro_arm_timeout(XrCoroutine *coro, int64_t timeout_ms) {
    XR_DCHECK(coro != NULL, "coro_arm_timeout: NULL coro");
    XR_DCHECK(timeout_ms > 0, "coro_arm_timeout: non-positive timeout");

    XrWorker *worker = xr_current_worker();
    if (!worker)
        return;

    xr_worker_add_sleep_timer(worker, coro, timeout_ms);
    XrRuntime *runtime = worker->p.runtime;
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.timeout_event_block_count);
    }
}

XrCoroBlockResult xr_coro_chan_send_resume(XrCoroutine *coro, XrSlotRef result_slot) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    int resume_status = xr_coro_resume_load(coro);
    if (resume_status == XR_RESUME_CHANNEL) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (resume_status == XR_RESUME_CHANNEL_CLOSED) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (resume_status == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_recv_resume(XrayIsolate *isolate, XrCoroutine *coro,
                                           XrSlotRef value_slot, XrSlotRef ok_slot) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    int resume_status = xr_coro_resume_load(coro);
    if (resume_status == XR_RESUME_CHANNEL) {
        XrValue value = xr_null();
        (void) xr_slot_load_value(value_slot, &value);
        value = xr_chan_copy_recv(isolate, value, coro);
        coro_finish_resume(coro);
        xr_slot_store_value(value_slot, value);
        xr_slot_store_value(ok_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, value, true);
    }
    if (resume_status == XR_RESUME_CHANNEL_CLOSED) {
        coro_finish_resume(coro);
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (resume_status == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_send(XrayIsolate *isolate, XrCoroutine *coro, XrChannel *ch,
                                    XrValue value, XrSlotRef result_slot, int64_t timeout_ms) {
    XR_DCHECK(ch != NULL, "xr_coro_chan_send: NULL channel");

    value = xr_chan_prepare_send(isolate, value);

    if (timeout_ms == 0) {
        if (xr_channel_try_send(ch, value)) {
            xr_slot_store_value(result_slot, xr_bool(true));
            return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
        }
        if (xr_channel_is_closed(ch)) {
            xr_slot_store_value(result_slot, xr_bool(false));
            return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
        }
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    if (coro && !xr_coro_ensure_ext(coro))
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    XrChanResult chan_result = xr_channel_send(ch, value, coro);
    if (chan_result == XR_CHAN_OK) {
        xr_slot_store_value(result_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (chan_result == XR_CHAN_CLOSED) {
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (chan_result == XR_CHAN_NO_CORO) {
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    if (chan_result == XR_CHAN_BLOCK) {
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_CHANNEL_SEND >> XR_CORO_WAIT_SHIFT);
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    xr_slot_store_value(result_slot, xr_bool(false));
    return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_recv(XrayIsolate *isolate, XrCoroutine *coro, XrChannel *ch,
                                    XrSlotRef value_slot, XrSlotRef ok_slot, int64_t timeout_ms) {
    XR_DCHECK(ch != NULL, "xr_coro_chan_recv: NULL channel");

    XrValue *recv_addr = xr_slot_value_address(value_slot);
    if (coro) {
        XrCoroExt *ext = xr_coro_ensure_ext(coro);
        if (!ext)
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        ext->recv_slot = recv_addr;
        ext->recv_slot_ref = value_slot;
    }

    if (timeout_ms == 0) {
        XrValue recv_val;
        if (xr_chan_try_recv(isolate, ch, &recv_val, coro)) {
            xr_slot_store_value(value_slot, recv_val);
            xr_slot_store_value(ok_slot, xr_bool(true));
            return block_result(XR_CORO_BLOCK_READY, recv_val, true);
        }
        if (xr_channel_is_closed(ch)) {
            xr_slot_store_value(value_slot, xr_null());
            xr_slot_store_value(ok_slot, xr_bool(false));
            return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
        }
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    XrValue recv_val;
    XrChanResult chan_result = xr_channel_recv(ch, &recv_val, coro);
    if (chan_result == XR_CHAN_OK) {
        recv_val = xr_chan_copy_recv(isolate, recv_val, coro);
        xr_slot_store_value(value_slot, recv_val);
        xr_slot_store_value(ok_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, recv_val, true);
    }
    if (chan_result == XR_CHAN_CLOSED) {
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (chan_result == XR_CHAN_NO_CORO) {
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    if (chan_result == XR_CHAN_BLOCK) {
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_CHANNEL_RECV >> XR_CORO_WAIT_SHIFT);
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    xr_slot_store_value(value_slot, xr_null());
    xr_slot_store_value(ok_slot, xr_bool(false));
    return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
}

static void coro_cancel_owned_timer(XrCoroutine *coro) {
    if (!coro || !coro->ext ||
        !atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed))
        return;

    XrWorker *worker = xr_current_worker();
    if (worker) {
        xr_worker_cancel_timer(worker, coro);
        return;
    }
    xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
    atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
}

XrCoroBlockResult xr_coro_await_task_resume(XrCoroutine *coro, XrTask *task) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    if (xr_coro_resume_load(coro) == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        if (wait)
            xr_await_wait_token_timeout(&wait->await_token);
        xr_task_unregister_await_waiters(coro);
        if (wait) {
            atomic_store_explicit(&wait->await_task, NULL, memory_order_relaxed);
            xr_await_wait_token_finish(&wait->await_token);
        }
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    if (task &&
        atomic_load_explicit(&task->await_state, memory_order_acquire) == XR_AWAIT_RESOLVED) {
        coro_cancel_owned_timer(coro);
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        if (wait) {
            atomic_store_explicit(&wait->await_task, NULL, memory_order_relaxed);
            xr_await_wait_token_finish(&wait->await_token);
            xr_timer_wait_token_finish(&wait->timer_token);
        }
        return block_result(XR_CORO_BLOCK_READY, task->result, true);
    }

    if (task && xr_task_is_done(task)) {
        coro_cancel_owned_timer(coro);
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        if (wait) {
            atomic_store_explicit(&wait->await_task, NULL, memory_order_relaxed);
            xr_await_wait_token_finish(&wait->await_token);
            xr_timer_wait_token_finish(&wait->timer_token);
        }
        if (atomic_load_explicit(&task->state, memory_order_acquire) == XR_TASK_COMPLETED) {
            return block_result(XR_CORO_BLOCK_READY, task->result, true);
        }
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrValue xr_coro_await_result_value(XrayIsolate *isolate, XrCoroutine *dst_coro, XrTask *task,
                                   bool discard_result) {
    if (discard_result || !task)
        return xr_null();

    XrValue result = task->result;
    if (dst_coro && isolate && xr_value_needs_copy(result)) {
        result = xr_deep_copy_to_coro(isolate, result, dst_coro);
        task->result = result;
    }
    return result;
}

static bool await_store_result(XrayIsolate *isolate, XrCoroutine *coro, XrTask *task,
                               XrSlotRef result_slot, bool discard_result, XrValue *out_value) {
    XrValue result = xr_coro_await_result_value(isolate, coro, task, discard_result);
    if (out_value)
        *out_value = result;
    return xr_slot_store_value(result_slot, result);
}

static XrCoroBlockResult coro_arm_await_wait(XrCoroutine *coro, XrTask *task, XrCoroWaitState *wait,
                                             int64_t timeout_ms) {
    atomic_store_explicit(&wait->await_task, task, memory_order_release);
    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
    if (timeout_ms > 0) {
        coro_arm_timeout(coro, timeout_ms);
    }
    xr_await_wait_token_commit(&wait->await_token);
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

static XrCoroBlockResult coro_register_single_await(XrCoroutine *coro, XrTask *task,
                                                    int64_t timeout_ms) {
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    xr_await_wait_token_prepare(&wait->await_token, task, -1);
    if (xr_task_register_await_waiter(task, coro, &wait->await_token, -1)) {
        return coro_arm_await_wait(coro, task, wait, timeout_ms);
    }

    atomic_store_explicit(&wait->await_task, NULL, memory_order_relaxed);
    xr_await_wait_token_finish(&wait->await_token);
    if (atomic_load_explicit(&task->state, memory_order_acquire) == XR_TASK_COMPLETED) {
        return block_result(XR_CORO_BLOCK_READY, task->result, true);
    }
    return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
}

static XrTaskAwaitNode *coro_prepare_multi_await_nodes(XrMultiAwaitWaitToken *token,
                                                       int task_count) {
    if (!token || task_count < 0)
        return NULL;

    XrTaskAwaitNode *nodes = token->inline_nodes;
    if (task_count > XR_MULTI_AWAIT_INLINE_TASKS) {
        if (token->heap_capacity < task_count) {
            XrTaskAwaitNode *new_nodes =
                (XrTaskAwaitNode *) xr_malloc((size_t) task_count * sizeof(XrTaskAwaitNode));
            if (!new_nodes)
                return NULL;
            if (token->heap_nodes)
                xr_free(token->heap_nodes);
            token->heap_nodes = new_nodes;
            token->heap_capacity = task_count;
        }
        nodes = token->heap_nodes;
    }

    memset(nodes, 0, (size_t) task_count * sizeof(XrTaskAwaitNode));
    token->nodes = nodes;
    token->node_count = task_count;
    return nodes;
}

XrCoroBlockResult xr_coro_await_task_resume_slot(XrayIsolate *isolate, XrCoroutine *coro,
                                                 XrTask *task, XrSlotRef result_slot,
                                                 bool discard_result) {
    XrCoroBlockResult result = xr_coro_await_task_resume(coro, task);
    if (result.kind == XR_CORO_BLOCK_READY) {
        XrValue value = xr_null();
        if (!await_store_result(isolate, coro, task, result_slot, discard_result, &value))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        result.value = value;
    } else if (result.kind == XR_CORO_BLOCK_TIMEOUT || result.kind == XR_CORO_BLOCK_CLOSED) {
        if (!xr_slot_store_value(result_slot, xr_null()))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return result;
}

XrCoroBlockResult xr_coro_await_task(XrCoroutine *coro, XrTask *task, int64_t timeout_ms) {
    XR_DCHECK(task != NULL, "xr_coro_await_task: NULL task");

    uint8_t task_state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (task_state == XR_TASK_COMPLETED) {
        return block_result(XR_CORO_BLOCK_READY, task->result, true);
    }
    if (task_state == XR_TASK_FAILED || task_state == XR_TASK_CANCELLED) {
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (timeout_ms == 0) {
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    return coro_register_single_await(coro, task, timeout_ms);
}

XrCoroBlockResult xr_coro_await_task_slot(XrayIsolate *isolate, XrCoroutine *coro, XrTask *task,
                                          XrSlotRef result_slot, int64_t timeout_ms,
                                          bool discard_result) {
    XrCoroBlockResult result = xr_coro_await_task(coro, task, timeout_ms);
    if (result.kind == XR_CORO_BLOCK_READY) {
        XrValue value = xr_coro_await_result_value(isolate, coro, task, discard_result);
        if (!xr_slot_store_value(result_slot, value))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        result.value = value;
    } else if (result.kind == XR_CORO_BLOCK_TIMEOUT || result.kind == XR_CORO_BLOCK_CLOSED ||
               result.kind == XR_CORO_BLOCK_NO_CORO) {
        if (!xr_slot_store_value(result_slot, xr_null()))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return result;
}

XrCoroBlockResult xr_coro_await_all_tasks(XrCoroutine *coro, XrArray *tasks) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    if (!tasks)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    int count = xr_array_size(tasks);
    bool all_done = true;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        if (!xr_task_is_done(xr_value_to_task(cv))) {
            all_done = false;
            break;
        }
    }

    if (all_done)
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);

    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    XrTaskAwaitNode *nodes = coro_prepare_multi_await_nodes(&wait->multi_await_token, count);
    if (count > 0 && !nodes)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    xr_multi_await_wait_token_prepare(&wait->multi_await_token, tasks, XR_MULTI_AWAIT_ALL, count);
    atomic_store(&wait->wait_count, 1);

    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;

        atomic_fetch_add(&wait->wait_count, 1);
        XrTask *task = xr_value_to_task(cv);
        if (!xr_task_register_await_node(task, coro, &nodes[j], j))
            atomic_fetch_sub(&wait->wait_count, 1);
    }

    int remaining = atomic_fetch_sub(&wait->wait_count, 1) - 1;
    if (remaining == 0) {
        xr_multi_await_wait_token_resolve(&wait->multi_await_token);
        xr_task_finish_await_waiters(coro);
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT);
    xr_multi_await_wait_token_commit(&wait->multi_await_token);
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

XrCoroBlockResult xr_coro_await_any_task(XrCoroutine *coro, XrArray *tasks, bool success_only) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    if (!tasks)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    int count = xr_array_size(tasks);
    int done_count = 0;
    int task_count = 0;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;

        task_count++;
        XrTask *task = xr_value_to_task(cv);
        if (xr_task_is_done(task)) {
            done_count++;
            if (!success_only || XR_IS_NULL(task->error))
                return block_result(XR_CORO_BLOCK_READY, task->result, true);
        }
    }

    if (task_count == 0 || (success_only && done_count == task_count))
        return block_result(XR_CORO_BLOCK_READY, xr_null(), false);

    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    XrTaskAwaitNode *nodes = coro_prepare_multi_await_nodes(&wait->multi_await_token, count);
    if (count > 0 && !nodes)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    xr_multi_await_wait_token_prepare(
        &wait->multi_await_token, tasks,
        success_only ? XR_MULTI_AWAIT_ANY_SUCCESS : XR_MULTI_AWAIT_ANY, count);
    atomic_store(&wait->any_done, false);
    atomic_store(&wait->wait_count, 1);

    int waiter_index = success_only ? -4 : -3;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;

        atomic_fetch_add(&wait->wait_count, 1);
        XrTask *task = xr_value_to_task(cv);
        if (!xr_task_register_await_node(task, coro, &nodes[j], waiter_index)) {
            if (!success_only || XR_IS_NULL(task->error)) {
                bool expected = false;
                if (atomic_compare_exchange_strong(&wait->any_done, &expected, true))
                    coro->result = task->result;
            }
            atomic_fetch_sub(&wait->wait_count, 1);
        }
    }

    int remaining = atomic_fetch_sub(&wait->wait_count, 1) - 1;
    if (atomic_load(&wait->any_done)) {
        xr_multi_await_wait_token_resolve(&wait->multi_await_token);
        xr_task_finish_await_waiters(coro);
        return block_result(XR_CORO_BLOCK_READY, coro->result, true);
    }
    if (success_only && remaining == 0) {
        xr_multi_await_wait_token_resolve(&wait->multi_await_token);
        xr_task_finish_await_waiters(coro);
        return block_result(XR_CORO_BLOCK_READY, xr_null(), false);
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_AWAIT_ANY >> XR_CORO_WAIT_SHIFT);
    xr_multi_await_wait_token_commit(&wait->multi_await_token);
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

XrCoroBlockResult xr_coro_sleep(XrCoroutine *coro, int64_t milliseconds) {
    if (milliseconds <= 0) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT);

    XrWorker *worker = xr_current_worker();
    if (!worker || !worker->p.timer_wheel) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    xr_worker_add_sleep_timer(worker, coro, milliseconds);
    if (!atomic_load_explicit(&ext->timer_active, memory_order_relaxed)) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

static XrSelectCase *coro_select_alloc_cases(XrRuntime *runtime, XrSelectStorage *storage,
                                             int case_slots) {
    if (case_slots <= XR_SELECT_INLINE_CASES) {
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.select_inline_alloc_count);
        }
        return storage->inline_cases;
    }

    if (storage->heap_capacity < case_slots) {
        XrSelectCase *new_cases =
            (XrSelectCase *) xr_malloc((size_t) case_slots * sizeof(XrSelectCase));
        if (!new_cases) {
            return NULL;
        }
        if (storage->heap_cases) {
            xr_free(storage->heap_cases);
        }
        storage->heap_cases = new_cases;
        storage->heap_capacity = case_slots;
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.select_heap_alloc_count);
        }
    }
    return storage->heap_cases;
}

static void coro_select_notify_enter(XrayIsolate *isolate, XrSelectWait *sw) {
    XrChannelDistHooks *dhooks =
        isolate ? (XrChannelDistHooks *) isolate->channel_dist_hooks : NULL;
    if (!dhooks || !dhooks->on_select_enter) {
        return;
    }

    for (int ci = 0; ci < sw->case_count; ci++) {
        XrChannel *ch = (XrChannel *) sw->cases[ci].channel;
        if (ch && ch->dist) {
            dhooks->on_select_enter(ch);
        }
    }
}

static void coro_select_arm_timer(XrWorker *worker, XrCoroutine *coro, XrChannel *timer_ch) {
    if (!worker || !coro || !timer_ch ||
        atomic_load_explicit(&timer_ch->timer_fired, memory_order_acquire)) {
        return;
    }

    int64_t now_ms = xr_monotonic_ticks();
    int64_t elapsed = now_ms - timer_ch->timer_start_ticks;
    int64_t remaining = timer_ch->timer_timeout_ms - elapsed;
    if (remaining < 1) {
        remaining = 1;
    }
    if (worker->p.timer_wheel) {
        coro_cancel_owned_timer(coro);
        xr_worker_add_sleep_timer(worker, coro, remaining);
    }
}

static int coro_select_recv_case_status(XrChannel *ch) {
    if (!ch)
        return XR_RESUME_OK;

    xr_channel_lock_observed(ch);
    bool ready = ch->buf_count > 0 || ch->sendq.first != NULL;
    bool closed = !ready && atomic_load_explicit(&ch->closed, memory_order_relaxed);
    xr_amutex_unlock(&ch->lock);
    if (ready)
        return XR_RESUME_CHANNEL;
    if (closed)
        return XR_RESUME_CHANNEL_CLOSED;
    return XR_RESUME_OK;
}

static bool coro_select_recheck_after_block(XrWorker *worker, XrCoroutine *coro, XrSelectWait *sw) {
    if (!worker || !coro || !sw)
        return false;

    for (int ci = 0; ci < sw->case_count; ci++) {
        XrChannel *ch = (XrChannel *) sw->cases[ci].channel;
        int status = coro_select_recv_case_status(ch);
        if (status == XR_RESUME_OK)
            continue;

        bool expected = false;
        if (!atomic_compare_exchange_strong(&sw->triggered, &expected, true))
            return false;

        xr_select_wait_resolve(sw);
        atomic_store_explicit(&sw->selected_index, ci, memory_order_release);
        atomic_store_explicit(&sw->selected_status, status, memory_order_release);
        xr_worker_unblock_select(worker, coro);
        xr_coro_clear_select_wait(coro);
        xr_coro_resume_store(coro, XR_RESUME_OK);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        xr_coro_transition_to_running(coro);
        return true;
    }

    return false;
}

XrCoroBlockResult xr_coro_select_block(XrayIsolate *isolate, XrCoroutine *coro,
                                       const XrValue *channel_values, int ch_count,
                                       const XrSlotRef *result_slots, int case_count) {
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrWorker *worker = xr_current_worker();
    if (!worker) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    XrRuntime *runtime = worker->p.runtime;
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.select_block_count);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    int case_slots = case_count > ch_count ? case_count : ch_count;
    if (case_slots <= 0) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }

    XrSelectStorage *storage = &ext->select_storage;
    XrSelectCase *cases = coro_select_alloc_cases(runtime, storage, case_slots);
    if (!cases) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    memset(cases, 0, (size_t) case_slots * sizeof(XrSelectCase));
    memset(&storage->wait, 0, sizeof(storage->wait));

    XrSelectWait *sw = &storage->wait;
    sw->cases = cases;
    sw->case_count = ch_count < case_count ? ch_count : case_count;
    sw->timer_channel = NULL;
    atomic_store(&sw->active, true);
    atomic_store(&sw->triggered, false);
    atomic_store(&sw->selected_index, -1);
    atomic_store(&sw->selected_status, 0);
    xr_select_wait_prepare(sw);

    XrChannel *timer_ch = NULL;
    for (int ci = 0; ci < ch_count && ci < sw->case_count; ci++) {
        XrValue ch_val = channel_values ? channel_values[ci] : xr_null();
        if (!xr_value_is_channel(ch_val)) {
            sw->cases[ci].channel = NULL;
            continue;
        }

        XrChannel *ch = xr_value_to_channel(ch_val);
        sw->cases[ci].channel = ch;
        sw->cases[ci].is_send = false;
        sw->cases[ci].result_slot = result_slots ? result_slots[ci] : xr_slot_none();
        sw->cases[ci].owner = coro;
        if (atomic_load(&ch->is_timer)) {
            timer_ch = ch;
        }
    }

    sw->timer_channel = timer_ch;

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SELECT >> XR_CORO_WAIT_SHIFT);
    coro_select_arm_timer(worker, coro, timer_ch);
    coro_select_notify_enter(isolate, sw);

    xr_worker_block_select(worker, coro, NULL, sw->case_count);
    if (coro_select_recheck_after_block(worker, coro, sw)) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

XrCoroBlockResult xr_coro_scope_enter(XrayIsolate *isolate, XrCoroutine *coro, uint8_t scope_mode) {
    XrCoroState *sched = isolate ? (XrCoroState *) isolate->vm.coro_state : NULL;
    if (!coro && !sched)
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);

    if (coro) {
        XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
        if (!wait)
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        atomic_store(&wait->wait_count, 0);
        atomic_store(&wait->any_done, false);
    }

    XrScopeContext *scope = (XrScopeContext *) xr_malloc(sizeof(XrScopeContext));
    if (!scope)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);

    atomic_store(&scope->count, 0);
    scope->mode = scope_mode;
    atomic_store(&scope->cancel_requested, false);
    atomic_init(&scope->child_lock, false);
    scope->first_error = xr_null();
    scope->first_error_is_value = false;
    scope->errors =
        (scope_mode == XR_SCOPE_SUPERVISOR && coro) ? xr_array_with_capacity(coro, 4) : NULL;
    scope->first_child = NULL;
    scope->owner = coro;
    if (coro) {
        scope->parent = coro->current_scope;
        coro->current_scope = scope;
    } else {
        scope->parent = sched->current_scope;
        sched->current_scope = scope;
    }
    return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
}

XrCoroBlockResult xr_coro_scope_exit(XrCoroutine *coro, uint8_t scope_mode) {
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrScopeContext *scope = coro->current_scope;
    if (!scope) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }

    if (atomic_load(&scope->count) > 0) {
        XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
        if (!wait)
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        xr_scope_wait_token_prepare(&wait->scope_token, scope);
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT);
        xr_scope_wait_token_commit(&wait->scope_token);
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    atomic_store(&wait->wait_count, 0);
    xr_scope_wait_token_finish(&wait->scope_token);

    if (scope_mode == XR_SCOPE_LINKED && !XR_IS_NULL(scope->first_error)) {
        XrValue err = scope->first_error;
        bool err_is_value = scope->first_error_is_value;
        coro->current_scope = scope->parent;
        xr_free(scope);
        return block_result(XR_CORO_BLOCK_ERROR, err, err_is_value);
    }

    XrValue supervisor_result = xr_null();
    if (scope_mode == XR_SCOPE_SUPERVISOR) {
        if (scope->errors && scope->errors->length > 0) {
            supervisor_result = xr_value_from_array(scope->errors);
        } else {
            XrArray *empty = xr_array_new(coro);
            supervisor_result = empty ? xr_value_from_array(empty) : xr_null();
        }
    }

    coro->current_scope = scope->parent;
    xr_free(scope);
    return block_result(XR_CORO_BLOCK_READY, supervisor_result, true);
}
