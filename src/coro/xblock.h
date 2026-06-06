/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock.h - Backend-neutral coroutine blocking helpers
 */

#ifndef XBLOCK_H
#define XBLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "../runtime/value/xvalue.h"
#include "xchannel.h"
#include "xslot_ref.h"

struct XrayIsolate;
struct XrArray;
struct XrCoroutine;
struct XrTask;

typedef enum {
    XR_CORO_BLOCK_NOT_RESUMED = 0,
    XR_CORO_BLOCK_READY,
    XR_CORO_BLOCK_BLOCKED,
    XR_CORO_BLOCK_CLOSED,
    XR_CORO_BLOCK_TIMEOUT,
    XR_CORO_BLOCK_NO_CORO,
    XR_CORO_BLOCK_ERROR
} XrCoroBlockKind;

typedef struct {
    XrCoroBlockKind kind;
    XrValue value;
    bool ok;
} XrCoroBlockResult;

XR_FUNC bool xr_slot_store_value(XrSlotRef slot, XrValue value);
XR_FUNC bool xr_slot_load_value(XrSlotRef slot, XrValue *out_value);
XR_FUNC XrValue *xr_slot_value_address(XrSlotRef slot);
XR_FUNC bool xr_coro_store_recv_value(struct XrCoroutine *coro, XrValue value);

XR_FUNC XrCoroBlockResult xr_coro_chan_send_resume(struct XrCoroutine *coro, XrSlotRef result_slot);
XR_FUNC XrCoroBlockResult xr_coro_chan_recv_resume(struct XrayIsolate *isolate,
                                                   struct XrCoroutine *coro, XrSlotRef value_slot,
                                                   XrSlotRef ok_slot);

XR_FUNC XrCoroBlockResult xr_coro_chan_send(struct XrayIsolate *isolate, struct XrCoroutine *coro,
                                            XrChannel *ch, XrValue value, XrSlotRef result_slot,
                                            int64_t timeout_ms);
XR_FUNC XrCoroBlockResult xr_coro_chan_recv(struct XrayIsolate *isolate, struct XrCoroutine *coro,
                                            XrChannel *ch, XrSlotRef value_slot, XrSlotRef ok_slot,
                                            int64_t timeout_ms);

XR_FUNC XrCoroBlockResult xr_coro_await_task_resume(struct XrCoroutine *coro, struct XrTask *task);
XR_FUNC XrCoroBlockResult xr_coro_await_task(struct XrCoroutine *coro, struct XrTask *task,
                                             int64_t timeout_ms);
XR_FUNC XrValue xr_coro_await_result_value(struct XrayIsolate *isolate,
                                           struct XrCoroutine *dst_coro, struct XrTask *task,
                                           bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_task_resume_slot(struct XrayIsolate *isolate,
                                                         struct XrCoroutine *coro,
                                                         struct XrTask *task, XrSlotRef result_slot,
                                                         bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_task_slot(struct XrayIsolate *isolate,
                                                  struct XrCoroutine *coro, struct XrTask *task,
                                                  XrSlotRef result_slot, int64_t timeout_ms,
                                                  bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_all_tasks(struct XrCoroutine *coro, struct XrArray *tasks);
XR_FUNC XrCoroBlockResult xr_coro_await_any_task(struct XrCoroutine *coro, struct XrArray *tasks,
                                                 bool success_only);

/* Runtime wait queues call this while holding the queue lock after wait
 * metadata is installed. The waiter becomes externally wakeable only after
 * the caller links it into the queue and releases that lock. */
XR_FUNC bool xr_coro_publish_locked_block(struct XrCoroutine *coro);

/* Backends call this after their continuation/frame state is quiescent and
 * before returning XR_CORO_RUN_BLOCKED to the scheduler. Channel helpers may
 * already publish BLOCKED under a channel lock; other wait helpers leave the
 * coroutine RUNNING until the backend reaches this boundary. */
XR_FUNC bool xr_coro_finalize_blocked_suspend(struct XrCoroutine *coro);
XR_FUNC void xr_coro_finish_backend_resume_tokens(struct XrCoroutine *coro, int resume_status);
XR_FUNC XrCoroBlockResult xr_coro_sleep(struct XrCoroutine *coro, int64_t milliseconds);
XR_FUNC XrCoroBlockResult xr_coro_select_block(struct XrayIsolate *isolate,
                                               struct XrCoroutine *coro,
                                               const XrValue *channel_values, int ch_count,
                                               const XrSlotRef *result_slots, int case_count);
XR_FUNC XrCoroBlockResult xr_coro_scope_enter(struct XrayIsolate *isolate, struct XrCoroutine *coro,
                                              uint8_t scope_mode);
XR_FUNC XrCoroBlockResult xr_coro_scope_exit(struct XrCoroutine *coro, uint8_t scope_mode);

#endif  // XBLOCK_H
