/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_flags.h - Coroutine state and flags management
 *
 * KEY CONCEPT:
 *   The whole scheduling state machine lives in ONE atomic uint32 (flags):
 *   a one-hot state field (READY/RUNNING/BLOCKED/DONE), the wait reason,
 *   and the mark flags. Every state transition is a single CAS; pure mark
 *   updates are a single fetch_or/fetch_and.
 *
 * WHY THIS DESIGN:
 *   - One word means one source of truth: a successful CAS both decides a
 *     race (wake claim, block publish) and makes the result visible. No
 *     shadow synchronization, no dual-field ordering rules.
 *   - Per transition this is exactly 1 atomic RMW. Concurrent mark writers
 *     (sysmon CANCEL_REQUESTED, GC marks) can force a CAS retry, but those
 *     writes are rare; retries are bounded and cheaper than the 2-3 extra
 *     RMWs the previous split-field protocol paid on every transition.
 *
 * FLAGS FIELD BIT LAYOUT (uint32_t atomic — single authority):
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *   | 31-23 |  22   | 21-20 | 19-16 | 15-12 | 11-8  |  7-4  |  3-0  |
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *   | resv  | SLAB  | marks | marks | marks | state | wait  | resv  |
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *
 *   bits 0-3:   Reserved
 *   bits 4-7:   Wait reason (NONE, CHANNEL_SEND, CHANNEL_RECV, AWAIT, ...)
 *   bits 8-11:  State field, one-hot (READY/RUNNING/BLOCKED/DONE);
 *               all bits clear = NONE (not yet scheduled / recycled)
 *   bit  12:    CANCELLED
 *   bit  13:    IN_RUNQ
 *   bit  14:    GC
 *   bit  15:    STARTED
 *   bit  16:    SUSPENDED
 *   bit  17:    MAIN
 *   bit  18:    DEAD
 *   bit  19:    NO_AUTO_FREE
 *   bit  20:    STACK_SCANNED
 *   bit  21:    CANCEL_REQUESTED (sysmon → worker)
 *   bit  22:    SLAB_STACK (arena slab allocation)
 *   bit  23:    DEFERRED_SUBMIT (executor waits for aggregate batch submission)
 *   bits 24-31: Reserved
 *
 * INVARIANT (one-hot state): at most one of READY/RUNNING/BLOCKED/DONE is
 * set at any time. Every primitive that sets a state bit clears the whole
 * state field in the same CAS, so the invariant holds by construction.
 */

#ifndef XCORO_FLAGS_H
#define XCORO_FLAGS_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

/* ========== State Enum (snapshot/diagnostics representation) ==========
 *
 * The authoritative encoding is the one-hot state field inside flags.
 * The enum form is kept for compact snapshots (XrCoroBlockSnapshot) and
 * debugger/DAP display. Convert with xr_state_to_flag / xr_flag_to_state.
 */

#define XR_CORO_STATE_NONE 0
#define XR_CORO_STATE_READY 1
#define XR_CORO_STATE_RUNNING 2
#define XR_CORO_STATE_BLOCKED 3
#define XR_CORO_STATE_DONE 4

/* ========== Wait Reason Encoding (flags bits 4-7) ========== */

#define XR_CORO_WAIT_SHIFT 4
#define XR_CORO_WAIT_MASK (0xF << XR_CORO_WAIT_SHIFT)

#define XR_CORO_WAIT_NONE (0 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_CHANNEL_SEND (1 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_CHANNEL_RECV (2 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_AWAIT (3 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_AWAIT_ALL (4 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_SLEEP (5 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_IO (6 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_SELECT (7 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_SCOPE (8 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_AWAIT_ANY (9 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_WORKQUEUE (10 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_RESULTGROUP (11 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_COUNTDOWN_LATCH (12 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_SEMAPHORE (13 << XR_CORO_WAIT_SHIFT)
#define XR_CORO_WAIT_EVENT_COUNT (14 << XR_CORO_WAIT_SHIFT)

/* ========== State Flags (shadow bits 8-11, mark bits 12+) ========== */

#define XR_CORO_FLG_READY (1 << 8)
#define XR_CORO_FLG_RUNNING (1 << 9)
#define XR_CORO_FLG_BLOCKED (1 << 10)
#define XR_CORO_FLG_DONE (1 << 11)
#define XR_CORO_FLG_CANCELLED (1 << 12)
#define XR_CORO_FLG_IN_RUNQ (1 << 13)
#define XR_CORO_FLG_GC (1 << 14)
#define XR_CORO_FLG_STARTED (1 << 15)
#define XR_CORO_FLG_SUSPENDED (1 << 16)
#define XR_CORO_FLG_MAIN (1 << 17)
#define XR_CORO_FLG_DEAD (1 << 18)
#define XR_CORO_FLG_NO_AUTO_FREE (1 << 19)
#define XR_CORO_FLG_STACK_SCANNED (1 << 20)
#define XR_CORO_FLG_CANCEL_REQUESTED (1 << 21)  // sysmon requests cancellation
#define XR_CORO_FLG_DEFERRED_SUBMIT (1u << 23)

// Mask for state shadow bits (READY|RUNNING|BLOCKED|DONE)
#define XR_CORO_STATE_FLAG_MASK                                                                    \
    (XR_CORO_FLG_READY | XR_CORO_FLG_RUNNING | XR_CORO_FLG_BLOCKED | XR_CORO_FLG_DONE)

/* ========== Internal: map between state enum and flag bits ========== */

static inline uint32_t xr_state_to_flag(uint8_t state) {
    switch (state) {
        case XR_CORO_STATE_READY:
            return XR_CORO_FLG_READY;
        case XR_CORO_STATE_RUNNING:
            return XR_CORO_FLG_RUNNING;
        case XR_CORO_STATE_BLOCKED:
            return XR_CORO_FLG_BLOCKED;
        case XR_CORO_STATE_DONE:
            return XR_CORO_FLG_DONE;
        default:
            return 0;
    }
}

static inline uint8_t xr_flag_to_state(uint32_t flag_bit) {
    if (flag_bit & XR_CORO_FLG_RUNNING)
        return XR_CORO_STATE_RUNNING;
    if (flag_bit & XR_CORO_FLG_BLOCKED)
        return XR_CORO_STATE_BLOCKED;
    if (flag_bit & XR_CORO_FLG_READY)
        return XR_CORO_STATE_READY;
    if (flag_bit & XR_CORO_FLG_DONE)
        return XR_CORO_STATE_DONE;
    return XR_CORO_STATE_NONE;
}

/* ========== State Operations (single-word authority) ========== */

/*
 * Load the full state word. One acquire load; the returned value carries
 * the one-hot state field, the wait reason, and all marks.
 */
#define xr_coro_flags_load(coro) atomic_load_explicit(&(coro)->flags, memory_order_acquire)

/*
 * Set flag bits. If `f` contains a state bit (READY/RUNNING/BLOCKED/DONE),
 * the whole state field is replaced in the same CAS so the one-hot
 * invariant holds. Pure mark sets are a single fetch_or.
 * The state-bit test folds away when `f` is a compile-time constant.
 */
static inline void xr_coro_flags_set_impl(_Atomic uint32_t *flags_ptr, uint32_t f) {
    if (f & XR_CORO_STATE_FLAG_MASK) {
        uint32_t old = atomic_load_explicit(flags_ptr, memory_order_relaxed);
        uint32_t neu;
        do {
            neu = (old & ~(uint32_t) XR_CORO_STATE_FLAG_MASK) | f;
        } while (!atomic_compare_exchange_weak_explicit(flags_ptr, &old, neu, memory_order_release,
                                                        memory_order_relaxed));
    } else {
        atomic_fetch_or_explicit(flags_ptr, f, memory_order_release);
    }
}

#define xr_coro_flags_set(coro, f) xr_coro_flags_set_impl(&(coro)->flags, (uint32_t) (f))

/*
 * Clear flag bits (marks or state). Clearing every state bit leaves the
 * state field at NONE — only init/recycle paths do that.
 */
#define xr_coro_flags_clear(coro, f)                                                               \
    atomic_fetch_and_explicit(&(coro)->flags, ~(uint32_t) (f), memory_order_release)

/*
 * Swap: clear some bits and set others in ONE atomic CAS.
 * When `set_mask` contains a state bit the whole state field is replaced
 * (regardless of `clear_mask`), preserving the one-hot invariant.
 * Concurrent mark updates (sysmon, GC) at worst force a retry.
 */
static inline void xr_coro_flags_swap_impl(_Atomic uint32_t *flags_ptr, uint32_t clear_mask,
                                           uint32_t set_mask) {
    if (set_mask & XR_CORO_STATE_FLAG_MASK)
        clear_mask |= XR_CORO_STATE_FLAG_MASK;
    uint32_t old = atomic_load_explicit(flags_ptr, memory_order_relaxed);
    uint32_t neu;
    do {
        neu = (old & ~clear_mask) | set_mask;
    } while (!atomic_compare_exchange_weak_explicit(flags_ptr, &old, neu, memory_order_release,
                                                    memory_order_relaxed));
}

#define xr_coro_flags_swap(coro, clear_mask, set_mask)                                             \
    xr_coro_flags_swap_impl(&(coro)->flags, (uint32_t) (clear_mask), (uint32_t) (set_mask))

/*
 * Check if any of the given bits is set. Single acquire load for state and
 * mark bits alike.
 */
#define xr_coro_flags_has(coro, f)                                                                 \
    ((atomic_load_explicit(&(coro)->flags, memory_order_acquire) & (uint32_t) (f)) != 0)

/* ========== State Transition Helpers ==========
 *
 * These thin wrappers make the intent of each state-machine edge explicit
 * at call sites. Each expands to exactly one CAS (xr_coro_flags_swap):
 *
 *   - transition_to_running : dequeue + begin execution
 *   - transition_to_ready   : voluntary yield / preemption
 *   - transition_to_blocked : channel send/recv/await wait point
 *     (unconditional publish; use xr_coro_try_transition_to_blocked when a
 *      concurrent waker may have claimed the coroutine already)
 *
 * CALLERS SHOULD PREFER THESE over direct xr_coro_flags_swap.
 */
#define xr_coro_transition_to_running(coro)                                                        \
    xr_coro_flags_swap((coro), XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED, XR_CORO_FLG_RUNNING)

#define xr_coro_transition_to_ready(coro)                                                          \
    xr_coro_flags_swap((coro), XR_CORO_FLG_RUNNING | XR_CORO_FLG_BLOCKED, XR_CORO_FLG_READY)

#define xr_coro_transition_to_blocked(coro)                                                        \
    xr_coro_flags_swap((coro), XR_CORO_FLG_RUNNING | XR_CORO_FLG_READY, XR_CORO_FLG_BLOCKED)

/*
 * RUNNING -> BLOCKED, but only if nobody woke the coroutine first.
 * Returns true when the coroutine is now (or already was) BLOCKED.
 * Returns false when the state moved on (READY: a waker claimed it;
 * DONE/NONE: terminal) — the caller must not treat it as parked.
 */
static inline bool xr_coro_try_transition_to_blocked_impl(_Atomic uint32_t *flags_ptr) {
    uint32_t old = atomic_load_explicit(flags_ptr, memory_order_acquire);
    for (;;) {
        if (old & XR_CORO_FLG_BLOCKED)
            return true;
        if (!(old & XR_CORO_FLG_RUNNING))
            return false;
        uint32_t neu = (old & ~(uint32_t) XR_CORO_STATE_FLAG_MASK) | XR_CORO_FLG_BLOCKED;
        if (atomic_compare_exchange_weak_explicit(flags_ptr, &old, neu, memory_order_release,
                                                  memory_order_acquire))
            return true;
    }
}

#define xr_coro_try_transition_to_blocked(coro)                                                    \
    xr_coro_try_transition_to_blocked_impl(&(coro)->flags)

/* ========== Unified Wake Claim ==========
 *
 * The single authoritative wake primitive. Atomically transitions a
 * coroutine BLOCKED -> READY via CAS and reports whether THIS caller won.
 * Every wake path (channel send/recv, channel close, select, timer, I/O,
 * task completion, scope) routes through it, and only the caller that wins
 * the CAS may enqueue the coroutine. Concurrent wakers therefore cannot
 * double-enqueue a coroutine: the race is eliminated by construction rather
 * than patched downstream with "if still BLOCKED" guards.
 *
 * Returns false when the coro is no longer BLOCKED (already woken / running /
 * done), in which case the caller must not touch the coroutine at all.
 */
static inline bool xr_coro_claim_wake_impl(_Atomic uint32_t *flags_ptr) {
    uint32_t old = atomic_load_explicit(flags_ptr, memory_order_acquire);
    for (;;) {
        if (!(old & XR_CORO_FLG_BLOCKED))
            return false;
        uint32_t neu =
            (old & ~(uint32_t) (XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK)) | XR_CORO_FLG_READY;
        if (atomic_compare_exchange_weak_explicit(flags_ptr, &old, neu, memory_order_acq_rel,
                                                  memory_order_acquire))
            return true;
    }
}

#define xr_coro_claim_wake(coro) xr_coro_claim_wake_impl(&(coro)->flags)

/* ========== Masked Field Update (race-free) ==========
 *
 * Atomically replace just the bits under `mask` with `value`, preserving
 * every other bit. A CAS loop is required because a plain
 * load -> set_field -> atomic_store sequence would clobber concurrent
 * single-bit updates from other threads (e.g. sysmon raising
 * CANCEL_REQUESTED, or a state shadow update). Used for wait-reason updates
 * while sysmon may be touching the same flags word.
 */
static inline void xr_coro_flags_update_field(_Atomic uint32_t *flags_ptr, uint32_t mask,
                                              uint32_t value) {
    uint32_t old = atomic_load_explicit(flags_ptr, memory_order_relaxed);
    uint32_t neu;
    do {
        neu = (old & ~mask) | (value & mask);
    } while (!atomic_compare_exchange_weak_explicit(flags_ptr, &old, neu, memory_order_release,
                                                    memory_order_relaxed));
}

/* ========== Wait Reason Operations ========== */

static inline int xr_coro_get_wait_reason(uint32_t flags) {
    return (flags & XR_CORO_WAIT_MASK) >> XR_CORO_WAIT_SHIFT;
}

static inline uint32_t xr_coro_set_wait_reason_flags(uint32_t flags, int reason) {
    return (flags & ~XR_CORO_WAIT_MASK) | (reason << XR_CORO_WAIT_SHIFT);
}

/* Atomically set the wait-reason sub-field (bits 4-7) only. `reason` is the
 * unshifted index (e.g. XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT). */
#define xr_coro_set_wait_reason(coro, reason)                                                      \
    xr_coro_flags_update_field(&(coro)->flags, XR_CORO_WAIT_MASK,                                  \
                               ((uint32_t) (reason) << XR_CORO_WAIT_SHIFT))

/* ========== State Check Macros ========== */

#define xr_coro_is_ready(coro) xr_coro_flags_has(coro, XR_CORO_FLG_READY)
#define xr_coro_is_running(coro) xr_coro_flags_has(coro, XR_CORO_FLG_RUNNING)
#define xr_coro_is_blocked(coro) xr_coro_flags_has(coro, XR_CORO_FLG_BLOCKED)
#define xr_coro_is_done_flag(coro) xr_coro_flags_has(coro, XR_CORO_FLG_DONE)
#define xr_coro_is_cancelled_flag(coro) xr_coro_flags_has(coro, XR_CORO_FLG_CANCELLED)
#define xr_coro_is_main_flag(coro) xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)
#define xr_coro_has_started(coro) xr_coro_flags_has(coro, XR_CORO_FLG_STARTED)
#define xr_coro_is_in_runq(coro) xr_coro_flags_has(coro, XR_CORO_FLG_IN_RUNQ)
#define xr_coro_is_gc(coro) xr_coro_flags_has(coro, XR_CORO_FLG_GC)
#define xr_coro_is_suspended(coro) xr_coro_flags_has(coro, XR_CORO_FLG_SUSPENDED)
#define xr_coro_is_dead(coro) xr_coro_flags_has(coro, XR_CORO_FLG_DEAD)

/* ========== Init ========== */

static inline uint32_t xr_coro_init_flags(bool is_main) {
    uint32_t flags = XR_CORO_FLG_READY;
    if (is_main) {
        flags |= XR_CORO_FLG_MAIN;
    }
    return flags;
}

/* ========== resume_status Accessors ==========
 * Use relaxed atomic: memory ordering is already provided by the surrounding
 * flags store(release) / load(acquire) pair.
 * These macros only ensure data-race-free access. */
#define xr_coro_resume_load(coro) atomic_load_explicit(&(coro)->resume_status, memory_order_relaxed)

#define xr_coro_resume_store(coro, val)                                                            \
    atomic_store_explicit(&(coro)->resume_status, (val), memory_order_relaxed)

#endif  // XCORO_FLAGS_H
