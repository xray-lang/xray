/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtimer_wheel.h - Hierarchical timer wheel implementation
 *
 * KEY CONCEPT:
 *   - Two-level timer wheel design (Soon + Later)
 *   - Per-Worker timer wheel (lock-free, no mutex)
 *   - Cross-worker cancellation via async canceled_queue
 *   - Timer ownership tracked by worker_id
 *
 * ERLANG DESIGN PRINCIPLES:
 *   - Timer wheel is PRIVATE to its owner worker
 *   - Only owner worker can directly manipulate the wheel
 *   - Cross-worker cancel: enqueue to owner's canceled_queue
 *   - Owner processes canceled_queue before each bump
 *
 * ZOMBIE OPTIMIZATION:
 *   - Cancel marks timer as ZOMBIE atomically (immediate cross-worker visibility)
 *   - find_next_timeout skips zombie timers (O(1) instead of O(n))
 *   - Async queue is for cleanup (garbage collection), not for cancel itself
 *   - Result: Cancel is O(1), find_next is O(1), cleanup is async
 */

#ifndef XTIMER_WHEEL_H
#define XTIMER_WHEEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../base/xdefs.h"

typedef struct XrCoroutine XrCoroutine;
typedef struct XrRuntime XrRuntime;
typedef struct XrWorker XrWorker;

/* ========== Timer Wheel Configuration ========== */

#define XR_TW_TICK_MS 1
#define XR_TW_SOON_WHEEL_BITS 14
#define XR_TW_SOON_WHEEL_SIZE (1 << XR_TW_SOON_WHEEL_BITS)
#define XR_TW_SOON_WHEEL_MASK (XR_TW_SOON_WHEEL_SIZE - 1)
#define XR_TW_SOON_WHEEL_FIRST_SLOT 0
#define XR_TW_SOON_WHEEL_END_SLOT (XR_TW_SOON_WHEEL_FIRST_SLOT + XR_TW_SOON_WHEEL_SIZE)
#define XR_TW_LATER_WHEEL_BITS 14
#define XR_TW_LATER_WHEEL_SIZE (1 << XR_TW_LATER_WHEEL_BITS)
#define XR_TW_LATER_WHEEL_MASK (XR_TW_LATER_WHEEL_SIZE - 1)
#define XR_TW_LATER_WHEEL_SHIFT (XR_TW_SOON_WHEEL_BITS - 1)
#define XR_TW_LATER_WHEEL_SLOT_SIZE ((int64_t) (1 << XR_TW_LATER_WHEEL_SHIFT))
#define XR_TW_LATER_WHEEL_POS_MASK (~(XR_TW_LATER_WHEEL_SLOT_SIZE - 1))
#define XR_TW_LATER_WHEEL_FIRST_SLOT XR_TW_SOON_WHEEL_SIZE
#define XR_TW_LATER_WHEEL_END_SLOT (XR_TW_LATER_WHEEL_FIRST_SLOT + XR_TW_LATER_WHEEL_SIZE)
#define XR_TW_SLOT_INACTIVE (-2)
#define XR_TW_SLOT_AT_ONCE (-1)
#define XR_TW_SCNT_BITS 9
#define XR_TW_SCNT_SIZE ((XR_TW_SOON_WHEEL_SIZE + XR_TW_LATER_WHEEL_SIZE) >> XR_TW_SCNT_BITS)
#define XR_TW_TICKS_WEEK (7LL * 24 * 60 * 60 * 1000)
#define XR_TW_MAX_TICKS INT64_MAX
#define XR_TW_BUMP_YIELD_LIMIT 10000
#define XR_TW_COST_SLOT 1
#define XR_TW_COST_SLOT_MOVE 5
#define XR_TW_COST_TIMEOUT 100

/* ========== Timer Node ========== */

typedef void (*XrTimeoutProc)(void *arg);

// Timer state flags (zombie marking)
#define XR_TIMER_STATE_ACTIVE 0x00  // Normal active timer
#define XR_TIMER_STATE_ZOMBIE 0x01  // Marked for deletion (lazy delete)

typedef struct XrTWheelTimer {
    struct XrTWheelTimer *prev;
    struct XrTWheelTimer *next;
    int64_t timeout_pos;
    int slot;
    XrTimeoutProc timeout;
    void *arg;
    int owner_worker_id;     // Worker that owns this timer
    _Atomic(uint8_t) state;  // Timer state (zombie flag, atomic for cross-worker)
} XrTWheelTimer;

/* ========== Canceled Timer Queue (MPSC) ========== */

// Node for canceled timer queue (lock-free MPSC)
typedef struct XrCanceledTimerNode {
    struct XrCanceledTimerNode *next;
    XrTWheelTimer *timer;
    XrCoroutine *coro;  // Associated coroutine (for cleanup)
} XrCanceledTimerNode;

// MPSC queue for cross-worker timer cancellation
typedef struct XrTimerCancelQueue {
    _Atomic(XrCanceledTimerNode *) head;  // Consumer reads from head
    _Atomic(XrCanceledTimerNode *) tail;  // Producers append to tail
    XrCanceledTimerNode stub;             // Stub node for empty queue
} XrTimerCancelQueue;

/* ========== Timer Wheel (Per-Worker, Lock-Free) ========== */

typedef struct XrTimerWheel {
    /* === Slot Array === */
    XrTWheelTimer *slots[1 + XR_TW_SOON_WHEEL_SIZE + XR_TW_LATER_WHEEL_SIZE];
    XrTWheelTimer **w;  // Pointer to current slot

    /* === Slot Counters === */
    int64_t scnt[XR_TW_SCNT_SIZE];
    int64_t bump_scnt[XR_TW_SCNT_SIZE];
    int64_t pos;  // Current wheel position
    int nto;      // Total active timers

    /* === Wheel State === */
    struct {
        int nto;
    } at_once;  // Immediate timeout slot
    struct {
        int64_t min_tpos;
        int nto;
    } soon;  // Soon wheel (1ms granularity)
    struct {
        int64_t min_tpos;
        int min_tpos_slot;
        int64_t pos;
        int nto;
    } later;  // Later wheel (8s granularity)

    /* === Bump State === */
    int yield_slot;
    int yield_slots_left;
    XrTWheelTimer sentinel;

    /* === Next Timeout Cache === */
    int true_next_timeout_time;
    int64_t next_timeout_pos;
    int64_t next_timeout_time;

    /* === Ownership === */
    int owner_worker_id;  // Worker that owns this timer wheel
    XrRuntime *runtime;

    /* === Canceled Timer Queue (cross-worker cancellation) === */
    XrTimerCancelQueue canceled_queue;
} XrTimerWheel;

/* ========== API ========== */

// Create timer wheel for a specific worker
XR_FUNC XrTimerWheel *xr_timer_wheel_create(XrRuntime *runtime, int owner_worker_id);
XR_FUNC void xr_timer_wheel_destroy(XrTimerWheel *tw);

// Set timer (must be called from owner worker)
XR_FUNC void xr_twheel_set_timer(XrTimerWheel *tw, XrTWheelTimer *timer, XrTimeoutProc timeout,
                                 void *arg, int64_t timeout_pos);

// Cancel timer - local only (must be called from owner worker)
// For cross-worker cancel, use xr_timer_queue_cancel()
XR_FUNC void xr_twheel_cancel_timer(XrTimerWheel *tw, XrTWheelTimer *timer);

// Bump timers (processes canceled_queue first, then triggers timeouts)
XR_FUNC void xr_bump_timers(XrTimerWheel *tw, int64_t curr_time);

// Query next timeout
XR_FUNC int64_t xr_check_next_timeout_time(XrTimerWheel *tw);
XR_FUNC int64_t *xr_get_next_timeout_reference(XrTimerWheel *tw);

/* ========== Cross-Worker Timer Cancellation () ========== */

// Initialize canceled timer queue
XR_FUNC void xr_timer_cancel_queue_init(XrTimerCancelQueue *cq);

// Queue a timer for cancellation (called from any worker)
// This is lock-free MPSC enqueue
XR_FUNC void xr_timer_queue_cancel(XrTimerWheel *target_tw, XrTWheelTimer *timer,
                                   XrCoroutine *coro);

// Process canceled queue (called by owner worker before bump)
// Returns number of timers processed
XR_FUNC int xr_timer_process_canceled_queue(XrTimerWheel *tw);

/* ========== Cancel Node Pool ========== */

// Allocate a cancel node from current worker's freelist, fallback to xr_malloc.
XR_FUNC XrCanceledTimerNode *xr_cancel_node_alloc(void);

// Return a cancel node to the owner worker's freelist (called during queue drain).
XR_FUNC void xr_cancel_node_free(XrCanceledTimerNode *node);

/* ========== Helpers ========== */

XR_FUNC int64_t xr_monotonic_ticks(void);
#define XR_MS_TO_TICKS(ms) (ms)
#define XR_TICKS_TO_MS(ticks) (ticks)

// Check if current thread is the owner of this timer wheel
static inline bool xr_twheel_is_owner(XrTimerWheel *tw, int worker_id) {
    return tw->owner_worker_id == worker_id;
}

#endif  // XTIMER_WHEEL_H
