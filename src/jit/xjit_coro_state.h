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
#include "../coro/xcoroutine.h"

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
 * JIT suspend state: saved registers across suspend/resume.
 * Heap-allocated on demand (lazy) to save 320 bytes per non-JIT coroutine.
 *
 * MEMORY LAYOUT (320 bytes = 40 * int64_t):
 *   +0    caller_saved[15]  x1-x15  (scratch regs, saved by XM_SUSPEND)
 *   +120  callee_saved[8]   x20-x27 (callee-saved, for cross-worker resume)
 *   +184  result            await/channel return value slot
 *   +192  result_tag        XR_TAG_* for result (written alongside result by waker)
 *   +200  spill[XM_SUSPEND_SPILL_MAX] spill slots bridging old->new stack frame
 */
typedef struct XrJitSuspendState {
    int64_t caller_saved[15];             // x1-x15
    int64_t callee_saved[8];              // x20-x27
    int64_t result;                       // await/channel result (written by block helper or waker)
    int64_t result_tag;                   // XR_TAG_* for result (resume writes to runtime_tags)
    int64_t spill[XM_SUSPEND_SPILL_MAX];  // spill slots (old frame -> suspend -> new frame)
} XrJitSuspendState;

struct XrJitCoroState {
    struct XrJitScratch *scratch;
    void *resume_entry;
    void *resume_proto;
    uint32_t suspend_id;
    uint32_t suspend_smap_id;
    XrJitSuspendState *suspend;
    bool try_mode;
};

#endif /* XJIT_CORO_STATE_H */
