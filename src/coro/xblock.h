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

struct XrVMRuntime;
struct XrArray;
struct XrCoroutine;
struct XrRuntimeCore;
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

typedef enum {
    XR_CORO_IO_WAIT_READY = 0,
    XR_CORO_IO_WAIT_BLOCKED,
    XR_CORO_IO_WAIT_YIELD,
    XR_CORO_IO_WAIT_TIMEOUT,
    XR_CORO_IO_WAIT_CANCELLED,
    XR_CORO_IO_WAIT_ERROR,
} XrCoroIoWaitKind;

typedef struct {
    XrCoroBlockKind kind;
    XrValue value;
    bool ok;
} XrCoroBlockResult;

typedef struct {
    uint8_t previous_state;
    bool active;
} XrCoroBlockSnapshot;

XR_FUNC bool xr_slot_store_value(XrSlotRef slot, XrValue value);
XR_FUNC bool xr_slot_load_value(XrSlotRef slot, XrValue *out_value);
XR_FUNC XrValue *xr_slot_value_address(XrSlotRef slot);
XR_FUNC bool xr_coro_store_recv_value(struct XrCoroutine *coro, XrValue value);

XR_FUNC XrCoroBlockResult xr_coro_chan_send_resume(struct XrCoroutine *coro, XrSlotRef result_slot);
XR_FUNC XrCoroBlockResult xr_coro_chan_recv_resume(struct XrCoroutine *coro, XrSlotRef value_slot,
                                                   XrSlotRef ok_slot);

XR_FUNC XrCoroBlockResult xr_coro_chan_send(struct XrCoroutine *coro, XrChannel *ch, XrValue value,
                                            XrSlotRef result_slot, int64_t timeout_ms);
XR_FUNC XrCoroBlockResult xr_coro_chan_send_transfer(struct XrCoroutine *coro, XrChannel *ch,
                                                     XrValue value, XrSlotRef result_slot,
                                                     int64_t timeout_ms, uint8_t transfer_mode);
/* deliver=true registers value_slot+ok_slot for waker-side delivery. Verified
 * channel payloads are already TRANSFER/shared, so the waker stores value+ok
 * directly and the coroutine resumes without replaying the operation. Timeout
 * variants, method-call and cfunc continuations pass deliver=false and keep the
 * replay/resume protocol. */
XR_FUNC XrCoroBlockResult xr_coro_chan_recv(struct XrCoroutine *coro, XrChannel *ch,
                                            XrSlotRef value_slot, XrSlotRef ok_slot,
                                            int64_t timeout_ms, bool deliver);

XR_FUNC XrCoroBlockResult xr_coro_await_task_resume(struct XrCoroutine *coro, struct XrTask *task);
XR_FUNC XrCoroBlockResult xr_coro_await_task(struct XrCoroutine *coro, struct XrTask *task,
                                             int64_t timeout_ms);
XR_FUNC XrValue xr_coro_await_result_value(struct XrRuntimeCore *core, struct XrCoroutine *dst_coro,
                                           struct XrTask *task, bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_task_resume_slot(struct XrCoroutine *coro,
                                                         struct XrTask *task, XrSlotRef result_slot,
                                                         bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_task_slot(struct XrCoroutine *coro, struct XrTask *task,
                                                  XrSlotRef result_slot, int64_t timeout_ms,
                                                  bool discard_result);
XR_FUNC XrCoroBlockResult xr_coro_await_submitted_task_slot(struct XrCoroutine *coro,
                                                            struct XrTask *task,
                                                            XrSlotRef result_slot,
                                                            int64_t timeout_ms,
                                                            bool discard_result);
XR_FUNC void xr_coro_submit_deferred_array_tasks(struct XrCoroutine *coro, struct XrArray *tasks);
XR_FUNC void xr_coro_submit_deferred_array_tasks_cached(struct XrCoroutine *coro,
                                                        struct XrArray *tasks);
XR_FUNC void xr_coro_submit_deferred_task_values(struct XrCoroutine *coro,
                                                 const XrValue *task_values, int task_count);
XR_FUNC XrCoroBlockResult xr_coro_await_all_tasks(struct XrCoroutine *coro, struct XrArray *tasks);
XR_FUNC XrCoroBlockResult xr_coro_await_all_tasks_resume(struct XrCoroutine *coro,
                                                         struct XrArray *tasks);
XR_FUNC XrCoroBlockResult xr_coro_await_all_task_values(struct XrCoroutine *coro,
                                                        const XrValue *task_values, int task_count);
XR_FUNC XrCoroBlockResult xr_coro_await_all_task_values_resume(struct XrCoroutine *coro,
                                                               const XrValue *task_values,
                                                               int task_count);
XR_FUNC XrCoroBlockResult xr_coro_await_any_task(struct XrCoroutine *coro, struct XrArray *tasks,
                                                 bool success_only);

/* Runtime wait queues call this after wait metadata is installed and before
 * the waiter becomes externally wakeable. The caller must own the queue by
 * lock or by single-worker ownership until linking is complete.
 *
 * Publication is the suspender's LAST touch: pending deferred spawns are
 * submitted here (before the transition), and the VM/AOT dispatch layers
 * complete all frame/replay state before calling into the blocking helper.
 * From the transition on, a waker or canceller may claim the coroutine and
 * resume it on another worker at any moment. */
XR_FUNC bool xr_coro_publish_wait_block(struct XrCoroutine *coro);

/* Submission paths use this when a coroutine must become BLOCKED before an
 * enqueue that can still fail. Rollback restores only state/shadow bits and
 * preserves concurrent non-state flag updates. */
XR_FUNC XrCoroBlockSnapshot xr_coro_begin_reversible_block(struct XrCoroutine *coro);
XR_FUNC void xr_coro_rollback_reversible_block(struct XrCoroutine *coro,
                                               XrCoroBlockSnapshot snapshot);

/* Backends call this before returning XR_CORO_RUN_BLOCKED to the scheduler.
 * Every published suspension (wait queues, channels, netpoll, select,
 * awaits) finished all owner-side work before the coroutine became
 * claimable, so this is a no-op for them — the coroutine may already be
 * running on another worker and must not be touched. Only the announced
 * worker-local timer park (xr_coro_sleep, via XrProc.suspend_park_pending)
 * still owns the coroutine here; for it this submits deferred spawns and
 * performs the RUNNING→BLOCKED transition. */
XR_FUNC bool xr_coro_finalize_blocked_suspend(struct XrCoroutine *coro);
XR_FUNC void xr_coro_finish_backend_resume_tokens(struct XrCoroutine *coro, int resume_status);
/* Register one backend-neutral netpoll wait. The caller must first attempt the
 * non-blocking socket operation and call this only after EAGAIN/EWOULDBLOCK.
 * A descriptor whose private deadline node belongs to another worker returns
 * YIELD and installs a one-shot targeted scheduler handoff; retrying on that
 * worker can then arm the descriptor without cross-wheel timer reuse. */
XR_FUNC XrCoroIoWaitKind xr_coro_io_wait(struct XrCoroutine *coro, int fd, int poll_mode,
                                         int64_t timeout_ms);
/* Consume the terminal token after a netpoll wake. IDLE is also READY: it is
 * the expected state after a targeted owner-worker handoff that did not arm a
 * wait yet. */
XR_FUNC XrCoroIoWaitKind xr_coro_io_wait_resume(struct XrCoroutine *coro);
XR_FUNC XrCoroBlockResult xr_coro_sleep(struct XrCoroutine *coro, int64_t milliseconds);
XR_FUNC XrCoroBlockResult xr_coro_select_block(struct XrCoroutine *coro,
                                               const XrValue *channel_values, int ch_count,
                                               const XrSlotRef *result_slots, int case_count);
XR_FUNC XrCoroBlockResult xr_coro_scope_enter(struct XrCoroutine *coro, uint8_t scope_mode);
XR_FUNC XrCoroBlockResult xr_coro_scope_exit(struct XrCoroutine *coro, uint8_t scope_mode);

#endif  // XBLOCK_H
