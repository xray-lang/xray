/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_coro_state.h - JIT coroutine suspend state layout
 *
 * KEY CONCEPT:
 *   JIT suspend/resume metadata belongs to the JIT backend. The scheduler
 *   sees an opaque XrJitCoroState pointer through the VM backend payload.
 */

#ifndef XJIT_CORO_STATE_H
#define XJIT_CORO_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "xjit_scratch.h"

/*
 * Capacity of XrJitSuspendState::spill[].
 *
 * Single source of truth consumed by JIT codegen and LSRA eligibility:
 *   - codegen stores/restores at most this many spill slots across a
 *     SUSPEND/RESUME transition;
 *   - a function containing an AWAIT/SUSPEND that requires more spill
 *     slots than this must be refused JIT compilation (otherwise the
 *     extra slots would be lost across the suspend bridge).
 *
 * If this value is raised, the _Static_assert in xm_offsets.h that checks
 * XM_SUSPEND_SPILL_OFF also needs to be revisited because sizeof the
 * containing struct changes.
 */
#define XM_SUSPEND_SPILL_MAX 15

/*
 * Channel recv status protocol across JIT suspend/resume.
 *
 * Direct fused recv projects a boolean from call_args[1]. Source-level
 * Channel.recv() wraps the payload into Recv<T> and keeps its enum state in
 * call_args[4]. Both scratch slots are worker-local, so blocking recv paths
 * must persist the status in XrJitSuspendState and restore it on resume.
 */
#define XR_JIT_CHAN_RECV_STATUS_SLOT 1
#define XR_JIT_CHAN_METHOD_RECV_STATUS_SLOT 4
#define XR_JIT_CHAN_RECV_VALUE 1
#define XR_JIT_CHAN_RECV_CLOSED 0
#define XR_JIT_CHAN_METHOD_RECV_VALUE 1
#define XR_JIT_CHAN_METHOD_RECV_CLOSED 2

/*
 * JIT suspend state: saved registers across suspend/resume.
 * Heap-allocated on demand (lazy) to save per-coroutine memory when JIT is unused.
 *
 * MEMORY LAYOUT (336 bytes = 42 * int64_t):
 *   +0    caller_saved[15]  x1-x15  (scratch regs, saved by XM_SUSPEND)
 *   +120  callee_saved[8]   x20-x27 (callee-saved, for cross-worker resume)
 *   +184  result            await/channel return value slot
 *   +192  result_tag        XR_TAG_* for result (written alongside result by waker)
 *   +200  result_status     paired status for recv-like operations
 *   +208  result_closed_status status to publish when a blocked recv wakes on close
 *   +216  spill[XM_SUSPEND_SPILL_MAX] spill slots bridging old->new stack frame
 */
typedef struct XrJitSuspendState {
    int64_t caller_saved[15];             // x1-x15
    int64_t callee_saved[8];              // x20-x27
    int64_t result;                       // await/channel result (written by block helper or waker)
    int64_t result_tag;                   // XR_TAG_* for result (resume writes to runtime_tags)
    int64_t result_status;                // recv status paired with result across suspend
    int64_t result_closed_status;         // recv status when a blocked recv resumes from close
    int64_t spill[XM_SUSPEND_SPILL_MAX];  // spill slots (old frame -> suspend -> new frame)
} XrJitSuspendState;

typedef struct XrJitCoroState XrJitCoroState;

struct XrJitCoroState {
    XrJitScratch *scratch;
    void *resume_entry;
    void *resume_proto;
    int32_t vm_call_base_offset;
    uint32_t suspend_id;
    uint32_t suspend_smap_id;
    XrJitSuspendState *suspend;
    bool try_mode;
};

#endif /* XJIT_CORO_STATE_H */
