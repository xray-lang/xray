/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_flags.h - Coroutine state and flags management (R4 split design)
 *
 * KEY CONCEPT:
 *   State is split into two fields for lock-free transitions:
 *   - coro_state (uint8_t atomic): mutually exclusive READY/RUNNING/BLOCKED/DONE
 *     State transitions use atomic_store (O(1), no CAS loop).
 *   - flags (uint32_t atomic): priority + wait_reason + mark flags
 *     Marks use atomic OR/AND (no CAS contention with state transitions).
 *
 *   The flags field also shadows state bits for backward compat with
 *   code that reads coro->flags directly.
 *
 * WHY THIS DESIGN:
 *   - Old design: single 32-bit field, all transitions use CAS loop.
 *     CAS fails when unrelated fields (e.g. sysmon sets CANCEL_REQUESTED)
 *     modify the same word concurrently — unnecessary retries on hot path.
 *   - New design: state transitions are single-byte atomic store (zero retry).
 *     Mark modifications (OR/AND) don't contend with state transitions.
 *
 * CORO_STATE FIELD (uint8_t atomic — authoritative):
 *   +-------+
 *   |  7-0  |
 *   +-------+
 *   | state |  NONE=0, READY=1, RUNNING=2, BLOCKED=3, DONE=4
 *   +-------+
 *   Mutually exclusive. Transitions: atomic_store (O(1), no CAS).
 *
 * FLAGS FIELD BIT LAYOUT (uint32_t atomic — marks + shadow):
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *   | 31-23 |  22   | 21-20 | 19-16 | 15-12 | 11-8  |  7-4  |  1-0  |
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *   | resv  | SLAB  | marks | marks | marks |shadow | wait  | prio  |
 *   +-------+-------+-------+-------+-------+-------+-------+-------+
 *
 *   bits 0-1:   Priority (LOW=0, NORMAL=1, HIGH=2)
 *   bits 2-3:   Reserved
 *   bits 4-7:   Wait reason (NONE, CHANNEL_SEND, CHANNEL_RECV, AWAIT, ...)
 *   bits 8-11:  State shadow (READY/RUNNING/BLOCKED/DONE)
 *               NOTE: shadow only, authoritative source is coro_state
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
 *   bits 23-31: Reserved
 */

#ifndef XCORO_FLAGS_H
#define XCORO_FLAGS_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

/* ========== Authoritative State (coro_state field) ========== */

#define XR_CORO_STATE_NONE 0
#define XR_CORO_STATE_READY 1
#define XR_CORO_STATE_RUNNING 2
#define XR_CORO_STATE_BLOCKED 3
#define XR_CORO_STATE_DONE 4

/* ========== Priority Encoding (flags bits 0-1) ========== */

#define XR_CORO_PRIO_SHIFT 0
#define XR_CORO_PRIO_MASK (0x3 << XR_CORO_PRIO_SHIFT)

#define XR_CORO_PRIO_LOW 0
#define XR_CORO_PRIO_NORMAL 1
#define XR_CORO_PRIO_HIGH 2

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
#define XR_CORO_FLG_SLAB_STACK (1 << 22)        // stack+frames from arena slab (don't free)

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

/* ========== State Operations (R4: route through coro_state) ========== */

/*
 * Load flags with state bits reconstructed from authoritative coro_state.
 * Returns a 32-bit value compatible with all existing flag-checking code.
 * Plain expression macro (no statement-expression / temp variable) so
 * MSVC compiles it; the two atomic loads are evaluated at distinct
 * points in the expanded expression, no shared temp is required.
 */
#define xr_coro_flags_load(coro)                                                                   \
    ((uint32_t) ((atomic_load_explicit(&(coro)->flags, memory_order_acquire) &                     \
                  ~(uint32_t) XR_CORO_STATE_FLAG_MASK) |                                           \
                 xr_state_to_flag(                                                                 \
                     atomic_load_explicit(&(coro)->coro_state, memory_order_acquire))))

/*
 * Set flag bits. If any state bit (READY/RUNNING/BLOCKED/DONE) is included,
 * update coro_state with atomic store (no CAS needed for exclusive states).
 * All bits are also OR'd into flags for shadow sync.
 */
#define xr_coro_flags_set(coro, f)                                                                 \
    do {                                                                                           \
        uint32_t _ff = (uint32_t) (f);                                                             \
        if (_ff & XR_CORO_STATE_FLAG_MASK) {                                                       \
            uint8_t _ns = xr_flag_to_state(_ff);                                                   \
            if (_ns)                                                                               \
                atomic_store_explicit(&(coro)->coro_state, _ns, memory_order_release);             \
        }                                                                                          \
        atomic_fetch_or_explicit(&(coro)->flags, _ff, memory_order_release);                       \
    } while (0)

/*
 * Clear flag bits. For state bits this only clears the shadow (authoritative
 * state is set by flags_set/flags_swap). For mark bits this is the real clear.
 */
#define xr_coro_flags_clear(coro, f)                                                               \
    atomic_fetch_and_explicit(&(coro)->flags, ~(uint32_t) (f), memory_order_release)

/*
 * Swap: clear some bits and set others atomically.
 * For state transitions (the hot path): single atomic store on coro_state.
 * Shadow sync on flags: AND then OR (non-CAS, lock-free).
 */
#define xr_coro_flags_swap(coro, clear_mask, set_mask)                                             \
    do {                                                                                           \
        uint32_t _set = (uint32_t) (set_mask);                                                     \
        if (_set & XR_CORO_STATE_FLAG_MASK) {                                                      \
            uint8_t _ns = xr_flag_to_state(_set);                                                  \
            if (_ns)                                                                               \
                atomic_store_explicit(&(coro)->coro_state, _ns, memory_order_release);             \
        }                                                                                          \
        atomic_fetch_and_explicit(&(coro)->flags, ~(uint32_t) (clear_mask), memory_order_relaxed); \
        atomic_fetch_or_explicit(&(coro)->flags, _set, memory_order_release);                      \
    } while (0)

/*
 * Check if flag is set. State bits check coro_state (authoritative).
 * Mark bits check flags field.
 * For pure state checks (f is a single state flag, 99% of cases):
 *   reads coro_state only → single byte load.
 * For pure mark checks: reads flags only.
 */
#define xr_coro_flags_has(coro, f)                                                                 \
    (((f) & XR_CORO_STATE_FLAG_MASK)                                                               \
         ? (xr_state_to_flag(atomic_load_explicit(&(coro)->coro_state, memory_order_acquire)) &    \
            (f)) != 0                                                                              \
         : (atomic_load_explicit(&(coro)->flags, memory_order_acquire) & (f)) != 0)

// Non-atomic fast variants (single-owner context only)
#define xr_coro_flags_set_fast(coro, f)                                                            \
    do {                                                                                           \
        uint32_t _ff = (uint32_t) (f);                                                             \
        if (_ff & XR_CORO_STATE_FLAG_MASK) {                                                       \
            uint8_t _ns = xr_flag_to_state(_ff);                                                   \
            if (_ns)                                                                               \
                (coro)->coro_state = _ns;                                                          \
        }                                                                                          \
        (coro)->flags |= _ff;                                                                      \
    } while (0)

#define xr_coro_flags_clear_fast(coro, f) ((coro)->flags &= ~(uint32_t) (f))

#define xr_coro_flags_has_fast(coro, f) (((coro)->flags & (f)) != 0)

// CAS on flags (used by xr_coro_ready). Also updates coro_state on success.
// Static inline helper avoids the GCC statement-expression macro form,
// which MSVC does not accept. Both fields' types are stable across the
// codebase, so the helper takes _Atomic pointers directly.
static inline bool xr_coro_flags_cas_impl(_Atomic uint32_t *flags_ptr,
                                          _Atomic uint8_t *state_ptr, uint32_t *expected,
                                          uint32_t desired) {
    bool ok = atomic_compare_exchange_strong_explicit(
        flags_ptr, expected, desired, memory_order_acq_rel, memory_order_acquire);
    if (ok && (desired & XR_CORO_STATE_FLAG_MASK)) {
        uint8_t ns = xr_flag_to_state(desired);
        if (ns)
            atomic_store_explicit(state_ptr, ns, memory_order_release);
    }
    return ok;
}

#define xr_coro_flags_cas(coro, expected, desired)                                                 \
    xr_coro_flags_cas_impl(&(coro)->flags, &(coro)->coro_state, (expected), (desired))

/* ========== State Transition Helpers ==========
 *
 * These thin wrappers make the intent of each state-machine edge explicit
 * at call sites that previously used raw xr_coro_flags_swap(...) with a
 * hand-rolled (clear_mask, set_mask) pair.  They preserve the exact memory
 * ordering of xr_coro_flags_swap (release on state, relaxed clear, release
 * set), so they are drop-in replacements.
 *
 * Transitions are named after the Go-scheduler style "<from>_to_<to>":
 *   - scheduled_to_running : dequeue + begin execution
 *                           (clears both READY and BLOCKED since the coro
 *                           may be woken from either path).
 *   - running_to_ready     : voluntary yield / preemption
 *   - running_to_blocked   : channel send/recv/await wait point
 *   - blocked_to_ready     : I/O wake, channel wake, timer fire
 *
 * CALLERS SHOULD PREFER THESE over direct xr_coro_flags_swap.
 */
#define xr_coro_transition_to_running(coro)                                                        \
    xr_coro_flags_swap((coro), XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED, XR_CORO_FLG_RUNNING)

#define xr_coro_transition_to_ready(coro)                                                          \
    xr_coro_flags_swap((coro), XR_CORO_FLG_RUNNING, XR_CORO_FLG_READY)

#define xr_coro_transition_to_blocked(coro)                                                        \
    xr_coro_flags_swap((coro), XR_CORO_FLG_RUNNING, XR_CORO_FLG_BLOCKED)

#define xr_coro_transition_wake(coro)                                                              \
    xr_coro_flags_swap((coro), XR_CORO_FLG_BLOCKED, XR_CORO_FLG_READY)

/* ========== Priority Operations ========== */

static inline int xr_coro_get_priority(uint32_t flags) {
    return (flags & XR_CORO_PRIO_MASK) >> XR_CORO_PRIO_SHIFT;
}

static inline uint32_t xr_coro_set_priority_flags(uint32_t flags, int prio) {
    return (flags & ~XR_CORO_PRIO_MASK) | ((prio << XR_CORO_PRIO_SHIFT) & XR_CORO_PRIO_MASK);
}

/* ========== Wait Reason Operations ========== */

static inline int xr_coro_get_wait_reason(uint32_t flags) {
    return (flags & XR_CORO_WAIT_MASK) >> XR_CORO_WAIT_SHIFT;
}

static inline uint32_t xr_coro_set_wait_reason_flags(uint32_t flags, int reason) {
    return (flags & ~XR_CORO_WAIT_MASK) | (reason << XR_CORO_WAIT_SHIFT);
}

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

static inline uint32_t xr_coro_init_flags(int priority, bool is_main) {
    uint32_t flags = XR_CORO_FLG_READY;
    flags |= (priority << XR_CORO_PRIO_SHIFT) & XR_CORO_PRIO_MASK;
    if (is_main) {
        flags |= XR_CORO_FLG_MAIN;
    }
    return flags;
}

/* ========== resume_status Accessors ==========
 * Use relaxed atomic: memory ordering is already provided by the surrounding
 * coro_state store(release) / load(acquire) pair.
 * These macros only ensure data-race-free access. */
#define xr_coro_resume_load(coro) atomic_load_explicit(&(coro)->resume_status, memory_order_relaxed)

#define xr_coro_resume_store(coro, val)                                                            \
    atomic_store_explicit(&(coro)->resume_status, (val), memory_order_relaxed)

#endif  // XCORO_FLAGS_H
