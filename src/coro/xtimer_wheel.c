/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtimer_wheel.c - Timer Wheel implementation (, lock-free)
 *
 * KEY CONCEPT:
 *   - Dual-layer timer wheel design for efficient timeout management
 *   - Per-Worker timer wheel (PRIVATE, no mutex needed)
 *   - Cross-worker cancellation via MPSC cancel stack
 *   - Only owner worker can directly manipulate the wheel
 *
 * ERLANG DESIGN:
 *   - Timer is bound to the worker that created it
 *   - Cancel from same worker: direct removal
 *   - Cancel from other worker: push to owner cancel stack
 *   - Owner drains cancel stack before each bump
 */

#include "xtimer_wheel.h"
#include "../base/xchecks.h"
#include "../os/os_time.h"
#include "xcoroutine.h"
#include "xworker.h"  // xr_current_worker (owner-thread assertions)
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "../base/xmalloc.h"

// ========== Helper Macros ==========

#define XR_TW_ASSERT(E) assert(E)

#define XR_TW_BUMP_LATER_WHEEL(tw) ((tw)->pos + XR_TW_LATER_WHEEL_SLOT_SIZE >= (tw)->later.pos)

// ========== Helper Functions ==========

// Get current time (clock ticks = milliseconds)
int64_t xr_monotonic_ticks(void) {
    return (int64_t) xr_time_monotonic_ms();
}

// Get slot count index
static inline int scnt_get_ix(int slot) {
    return slot >> XR_TW_SCNT_BITS;
}

// Increment slot count
static inline void scnt_inc(int64_t *scnt, int slot) {
    scnt[slot >> XR_TW_SCNT_BITS]++;
}

// Decrement slot count
static inline void scnt_dec(int64_t *scnt, int slot) {
    scnt[slot >> XR_TW_SCNT_BITS]--;
    XR_TW_ASSERT(scnt[slot >> XR_TW_SCNT_BITS] >= 0);
}

static inline void scnt_ix_dec(int64_t *scnt, int six) {
    scnt[six]--;
    XR_TW_ASSERT(scnt[six] >= 0);
}

// Find next non-empty slot in wheel
static inline void scnt_wheel_next(int *slotp, int *leftp, int64_t *posp, int *sixp, int64_t *scnt,
                                   int first_slot, int end_slot, int64_t slot_sz) {
    int slot = *slotp;
    int left = *leftp;
    int ix;

    XR_TW_ASSERT(*leftp >= 0);

    left--;
    slot++;
    if (slot == end_slot)
        slot = first_slot;
    ix = slot >> XR_TW_SCNT_BITS;

    while (!scnt[ix] && left > 0) {
        int diff, old_slot = slot;
        ix++;
        slot = (ix << XR_TW_SCNT_BITS);
        diff = slot - old_slot;
        if (left < diff) {
            slot = old_slot + left;
            diff = left;
        }
        if (slot < end_slot)
            left -= diff;
        else {
            left -= end_slot - old_slot;
            slot = first_slot;
            ix = slot >> XR_TW_SCNT_BITS;
        }
    }

    XR_TW_ASSERT(left >= -1);

    if (posp)
        *posp += slot_sz * ((int64_t) (*leftp - left));
    if (sixp)
        *sixp = slot >> XR_TW_SCNT_BITS;
    *leftp = left;
    *slotp = slot;
}

static inline void scnt_soon_wheel_next(int *slotp, int *leftp, int64_t *posp, int *sixp,
                                        int64_t *scnt) {
    scnt_wheel_next(slotp, leftp, posp, sixp, scnt, XR_TW_SOON_WHEEL_FIRST_SLOT,
                    XR_TW_SOON_WHEEL_END_SLOT, 1);
}

static inline void scnt_later_wheel_next(int *slotp, int *leftp, int64_t *posp, int *sixp,
                                         int64_t *scnt) {
    scnt_wheel_next(slotp, leftp, posp, sixp, scnt, XR_TW_LATER_WHEEL_FIRST_SLOT,
                    XR_TW_LATER_WHEEL_END_SLOT, XR_TW_LATER_WHEEL_SLOT_SIZE);
}

static inline void timer_refresh_runtime_mask(XrTimerWheel *tw) {
    if (!tw)
        return;
    xr_runtime_set_timer_pending(tw->runtime, tw->owner_worker_id,
                                 tw->nto > 0 || tw->yield_slot != XR_TW_SLOT_INACTIVE);
}

// Calculate Soon Wheel slot
static inline int soon_slot(int64_t soon_pos) {
    int64_t slot = soon_pos;
    slot &= XR_TW_SOON_WHEEL_MASK;
    XR_TW_ASSERT(XR_TW_SOON_WHEEL_FIRST_SLOT <= slot);
    XR_TW_ASSERT(slot < XR_TW_SOON_WHEEL_END_SLOT);
    return (int) slot;
}

// Calculate Later Wheel slot
static inline int later_slot(int64_t later_pos) {
    int64_t slot = later_pos;
    slot >>= XR_TW_LATER_WHEEL_SHIFT;
    slot &= XR_TW_LATER_WHEEL_MASK;
    slot += XR_TW_LATER_WHEEL_FIRST_SLOT;
    XR_TW_ASSERT(XR_TW_LATER_WHEEL_FIRST_SLOT <= slot);
    XR_TW_ASSERT(slot < XR_TW_LATER_WHEEL_END_SLOT);
    return (int) slot;
}

// Insert timer into slot
static inline void insert_timer_into_slot(XrTimerWheel *tw, int slot, XrTWheelTimer *p) {
    XR_TW_ASSERT(XR_TW_SLOT_AT_ONCE <= slot && slot < XR_TW_LATER_WHEEL_END_SLOT);

    p->slot = slot;
    if (!tw->w[slot]) {
        tw->w[slot] = p;
        p->next = p;
        p->prev = p;
    } else {
        XrTWheelTimer *next, *prev;
        next = tw->w[slot];
        prev = next->prev;
        p->next = next;
        p->prev = prev;
        prev->next = p;
        next->prev = p;
    }

    if (slot == XR_TW_SLOT_AT_ONCE) {
        tw->at_once.nto++;
    } else {
        int64_t tpos = p->timeout_pos;
        if (slot < XR_TW_SOON_WHEEL_END_SLOT) {
            XR_TW_ASSERT(p->timeout_pos < tw->pos + XR_TW_SOON_WHEEL_SIZE);
            tw->soon.nto++;
            if (tw->soon.min_tpos > tpos)
                tw->soon.min_tpos = tpos;
        } else {
            XR_TW_ASSERT(p->timeout_pos >= tw->pos + XR_TW_SOON_WHEEL_SIZE);
            tw->later.nto++;
            if (tw->later.min_tpos > tpos) {
                tw->later.min_tpos = tpos;
                tw->later.min_tpos_slot = slot;
            }
        }
        scnt_inc(tw->scnt, slot);
    }
}

// Remove timer from slot
static inline void remove_timer(XrTimerWheel *tw, XrTWheelTimer *p) {
    int slot = p->slot;
    int empty_slot;

    XR_TW_ASSERT(slot != XR_TW_SLOT_INACTIVE);
    XR_TW_ASSERT(XR_TW_SLOT_AT_ONCE <= slot && slot < XR_TW_LATER_WHEEL_END_SLOT);

    if (p->next == p) {
        XR_TW_ASSERT(tw->w[slot] == p);
        tw->w[slot] = NULL;
        empty_slot = 1;
    } else {
        if (tw->w[slot] == p)
            tw->w[slot] = p->next;
        p->prev->next = p->next;
        p->next->prev = p->prev;
        empty_slot = 0;
    }

    if (slot == XR_TW_SLOT_AT_ONCE) {
        XR_TW_ASSERT(tw->at_once.nto > 0);
        tw->at_once.nto--;
    } else {
        scnt_dec(tw->scnt, slot);
        if (slot < XR_TW_SOON_WHEEL_END_SLOT) {
            // If removed timer's timeout_pos <= min_tpos, invalidate min_tpos cache
            // so find_next_timeout will recalculate the actual minimum
            if (p->timeout_pos <= tw->soon.min_tpos) {
                // Set to current pos to force full scan on next find_next_timeout
                tw->soon.min_tpos = tw->pos;
            }
            if (tw->true_next_timeout_time && p->timeout_pos == xr_tw_next_pos(tw) &&
                tw->yield_slot == XR_TW_SLOT_INACTIVE) {
                tw->true_next_timeout_time = 0;
            }
            if (--tw->soon.nto == 0)
                tw->soon.min_tpos = XR_TW_MAX_TICKS;
        } else {
            if (empty_slot && tw->true_next_timeout_time && tw->later.min_tpos_slot == slot) {
                int64_t tpos = tw->later.min_tpos;
                tpos &= XR_TW_LATER_WHEEL_POS_MASK;
                tpos -= XR_TW_LATER_WHEEL_SLOT_SIZE;
                if (tpos == xr_tw_next_pos(tw) && tw->yield_slot == XR_TW_SLOT_INACTIVE)
                    tw->true_next_timeout_time = 0;
            }
            if (--tw->later.nto == 0) {
                tw->later.min_tpos = XR_TW_MAX_TICKS;
                tw->later.min_tpos_slot = XR_TW_LATER_WHEEL_END_SLOT;
            }
        }
    }
    p->slot = XR_TW_SLOT_INACTIVE;
}

static bool timer_is_in_slot(XrTimerWheel *tw, XrTWheelTimer *p, int slot) {
    if (slot < XR_TW_SLOT_AT_ONCE || slot >= XR_TW_LATER_WHEEL_END_SLOT)
        return false;
    XrTWheelTimer *first = tw->w[slot];
    if (!first)
        return false;
    if (p->next == p || p->prev == p)
        return first == p && p->next == p && p->prev == p;
    if (!p->next || !p->prev || p->next->prev != p || p->prev->next != p)
        return false;
    return true;
}

// Trigger timer
static inline void timeout_timer(XrTimerWheel *tw, XrTWheelTimer *p) {
    XrTimeoutProc timeout;
    void *arg;

    p->slot = XR_TW_SLOT_INACTIVE;
    timeout = p->timeout;
    arg = p->arg;
    if (tw && tw->runtime) {
        xr_sched_metric_inc(tw->runtime, &tw->runtime->sched_stats.timer_fire_count);
    }
    if (timeout) {
        (*timeout)(arg);
    }
}

// Find next timeout
static int64_t find_next_timeout(XrTimerWheel *tw) {
    int slot, slots;
    int true_min_timeout = 0;
    int64_t min_timeout_pos;

    XR_TW_ASSERT(tw->pos + XR_TW_LATER_WHEEL_SLOT_SIZE < tw->later.pos &&
                 tw->later.pos <= tw->pos + XR_TW_SOON_WHEEL_SIZE);
    XR_TW_ASSERT(tw->at_once.nto == 0);
    XR_TW_ASSERT(tw->nto == tw->soon.nto + tw->later.nto);
    XR_TW_ASSERT(tw->yield_slot == XR_TW_SLOT_INACTIVE);

    if (tw->nto == 0) {
        int64_t curr_time = xr_monotonic_ticks();
        tw->pos = min_timeout_pos = curr_time;
        tw->later.pos = min_timeout_pos + XR_TW_SOON_WHEEL_SIZE;
        tw->later.pos &= XR_TW_LATER_WHEEL_POS_MASK;
        min_timeout_pos += XR_TW_TICKS_WEEK;
        goto done;
    }

    XR_TW_ASSERT(tw->soon.nto || tw->later.nto);

    if (!tw->soon.nto) {
        int64_t tpos, min_tpos;

        min_tpos = tw->later.min_tpos & XR_TW_LATER_WHEEL_POS_MASK;

        if (min_tpos <= tw->later.pos) {
            tpos = tw->later.pos;
            slots = XR_TW_LATER_WHEEL_SIZE;
        } else {
            int64_t tmp;
            tmp = min_tpos - tw->later.pos;
            tmp /= XR_TW_LATER_WHEEL_SLOT_SIZE;
            if (tmp >= XR_TW_LATER_WHEEL_SIZE) {
                min_timeout_pos = min_tpos - XR_TW_LATER_WHEEL_SLOT_SIZE;
                goto done;
            }
            tpos = min_tpos;
            slots = XR_TW_LATER_WHEEL_SIZE - ((int) tmp);
        }

        slot = later_slot(tpos);

        if (tw->w[slot])
            true_min_timeout = 1;
        else
            scnt_later_wheel_next(&slot, &slots, &tpos, NULL, tw->scnt);

        tw->later.min_tpos = tpos;
        tw->later.min_tpos_slot = slot;

        tpos -= XR_TW_LATER_WHEEL_SLOT_SIZE;
        min_timeout_pos = tpos;
    } else {
        int64_t tpos;

        min_timeout_pos = tw->pos + XR_TW_SOON_WHEEL_SIZE;

        if (tw->later.min_tpos > (tw->later.pos + 2 * XR_TW_LATER_WHEEL_SLOT_SIZE)) {
            // Empty
        } else {
            int fslot;
            tpos = tw->later.pos;
            tpos -= XR_TW_LATER_WHEEL_SLOT_SIZE;
            fslot = later_slot(tw->later.pos);
            if (tw->w[fslot])
                min_timeout_pos = tpos;
            else {
                tpos += XR_TW_LATER_WHEEL_SLOT_SIZE;
                if (tpos < min_timeout_pos) {
                    fslot++;
                    if (fslot == XR_TW_LATER_WHEEL_END_SLOT)
                        fslot = XR_TW_LATER_WHEEL_FIRST_SLOT;
                    if (tw->w[fslot])
                        min_timeout_pos = tpos;
                }
            }
        }

        if (tw->soon.min_tpos <= tw->pos) {
            tpos = tw->pos;
            slots = XR_TW_SOON_WHEEL_SIZE;
        } else {
            int64_t tmp;
            tmp = tw->soon.min_tpos - tw->pos;
            XR_TW_ASSERT(XR_TW_SOON_WHEEL_SIZE > tmp);
            slots = XR_TW_SOON_WHEEL_SIZE - ((int) tmp);
            tpos = tw->soon.min_tpos;
        }

        slot = soon_slot(tpos);

        while (tpos < min_timeout_pos) {
            XrTWheelTimer *first = tw->w[slot];
            if (first) {
                // zombie check: skip zombie timers immediately (O(1))
                // Zombie timers are marked atomically by cross-worker cancel,
                // so we can safely skip them without traversing the entire slot.
                if (atomic_load_explicit(&first->state, memory_order_acquire) !=
                    XR_TIMER_STATE_ZOMBIE) {
                    // Non-zombie timer found at slot head - use its timeout_pos
                    // Note: If min_tpos was stale and first->timeout_pos > tpos,
                    // it just means we wake up slightly early, which is harmless.
                    min_timeout_pos = first->timeout_pos;
                    break;
                }
                // First timer is zombie, continue searching (rare case)
            }
            scnt_soon_wheel_next(&slot, &slots, &tpos, NULL, tw->scnt);
        }

        tw->soon.min_tpos = min_timeout_pos;
        true_min_timeout = 1;
    }

done: {
    int64_t timeout_pos_limit;

    timeout_pos_limit = tw->pos + XR_TW_TICKS_WEEK;
    if (min_timeout_pos > timeout_pos_limit) {
        min_timeout_pos = timeout_pos_limit;
        true_min_timeout = 0;
    }

    xr_tw_set_next_pos(tw, min_timeout_pos);
    tw->next_timeout_time = min_timeout_pos;
    tw->true_next_timeout_time = true_min_timeout;

    return min_timeout_pos;
}
}

// Process Later Wheel
static int bump_later_wheel(XrTimerWheel *tw, int *ycount_p) {
    int64_t cpos = tw->pos;
    int64_t later_pos = tw->later.pos;
    int ycount = *ycount_p;
    int slots, fslot, scnt_ix;
    int64_t *scnt, *bump_scnt;

    scnt = &tw->scnt[0];
    bump_scnt = &tw->bump_scnt[0];

    if (tw->yield_slot >= XR_TW_LATER_WHEEL_FIRST_SLOT) {
        fslot = tw->yield_slot;
        scnt_ix = scnt_get_ix(fslot);
        slots = tw->yield_slots_left;
        tw->yield_slot = XR_TW_SLOT_INACTIVE;
        goto restart_yielded_slot;
    } else {
        int64_t end_later_pos, tmp_slots, min_tpos;

        min_tpos = tw->later.min_tpos & XR_TW_LATER_WHEEL_POS_MASK;
        end_later_pos = cpos + XR_TW_SOON_WHEEL_SIZE;
        end_later_pos &= XR_TW_LATER_WHEEL_POS_MASK;

        if (min_tpos > later_pos) {
            if (min_tpos > end_later_pos) {
                tw->later.pos = end_later_pos;
                goto done;
            }
            later_pos = min_tpos;
        }

        tmp_slots = end_later_pos;
        tmp_slots -= later_pos;
        tmp_slots /= XR_TW_LATER_WHEEL_SLOT_SIZE;
        if (tmp_slots < XR_TW_LATER_WHEEL_SIZE)
            slots = (int) tmp_slots;
        else
            slots = XR_TW_LATER_WHEEL_SIZE;

        fslot = later_slot(later_pos);
        scnt_ix = scnt_get_ix(fslot);

        tw->later.pos = end_later_pos;
    }

    while (slots > 0) {
        XrTWheelTimer *p;

        ycount -= XR_TW_COST_SLOT;

        p = tw->w[fslot];

        if (p) {
            if (p->next == p) {
                XR_TW_ASSERT(tw->sentinel.next == &tw->sentinel);
                XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
            } else {
                tw->sentinel.next = p->next;
                tw->sentinel.prev = p->prev;
                tw->sentinel.next->prev = &tw->sentinel;
                tw->sentinel.prev->next = &tw->sentinel;
            }
            tw->w[fslot] = NULL;

            while (1) {
                int64_t tpos = p->timeout_pos;

                // Timer may have been cancelled or reused during yield, skip if slot doesn't match
                if (p->slot != fslot) {
                    goto restart_yielded_slot;
                }

                if (--tw->later.nto == 0) {
                    tw->later.min_tpos = XR_TW_MAX_TICKS;
                    tw->later.min_tpos_slot = XR_TW_LATER_WHEEL_END_SLOT;
                }
                scnt_ix_dec(scnt, scnt_ix);

                // Set slot = INACTIVE immediately, allow safe external reuse
                p->slot = XR_TW_SLOT_INACTIVE;

                // check zombie before processing
                if (atomic_load_explicit(&p->state, memory_order_acquire) ==
                    XR_TIMER_STATE_ZOMBIE) {
                    // Zombie timer, clean up without callback
                    atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE, memory_order_release);
                    scnt_ix_dec(bump_scnt, scnt_ix);
                    tw->nto--;
                } else if (tpos >= tw->later.pos + XR_TW_LATER_WHEEL_SLOT_SIZE) {
                    insert_timer_into_slot(tw, fslot, p);
                    ycount -= XR_TW_COST_SLOT_MOVE;
                } else {
                    scnt_ix_dec(bump_scnt, scnt_ix);
                    XR_TW_ASSERT(tpos < cpos + XR_TW_SOON_WHEEL_SIZE);
                    if (tpos > cpos) {
                        insert_timer_into_slot(tw, soon_slot(tpos), p);
                        ycount -= XR_TW_COST_SLOT_MOVE;
                    } else {
                        timeout_timer(tw, p);
                        tw->nto--;
                        ycount -= XR_TW_COST_TIMEOUT;
                    }
                }

            restart_yielded_slot:

                p = tw->sentinel.next;
                if (p == &tw->sentinel) {
                    XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
                    break;
                }

                if (ycount < 0) {
                    tw->yield_slot = fslot;
                    tw->yield_slots_left = slots;
                    *ycount_p = 0;
                    return 1;
                }

                tw->sentinel.next = p->next;
                p->next->prev = &tw->sentinel;
            }
        }

        scnt_later_wheel_next(&fslot, &slots, NULL, &scnt_ix, bump_scnt);
    }

done:
    *ycount_p = ycount;
    return 0;
}

// ========== Canceled Timer Stack Operations (MPSC) ==========

// Initialize canceled timer stack.
void xr_timer_cancel_queue_init(XrTimerCancelQueue *cq) {
    XR_DCHECK(cq != NULL, "timer_cancel_queue_init: NULL cq");
    atomic_store_explicit(&cq->head, NULL, memory_order_relaxed);
    XR_DCHECK(atomic_load(&cq->head) == NULL, "timer_cancel_queue_init: head invariant");
}

// Push a timer cancellation request from any worker.
void xr_timer_queue_cancel(XrTimerWheel *target_tw, XrTWheelTimer *timer) {
    XR_DCHECK(target_tw != NULL, "timer_queue_cancel: NULL target_tw");
    XR_DCHECK(timer != NULL, "timer_queue_cancel: NULL timer");
    if (target_tw->runtime) {
        xr_sched_metric_inc(target_tw->runtime,
                            &target_tw->runtime->sched_stats.timer_cancel_remote_count);
    }

    uint8_t expected = XR_TIMER_STATE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(&timer->state, &expected, XR_TIMER_STATE_ZOMBIE,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        if (target_tw->runtime) {
            xr_sched_metric_inc(target_tw->runtime,
                                &target_tw->runtime->sched_stats.timer_cancel_duplicate_count);
        }
        return;
    }

    XrTWheelTimer *head =
        atomic_load_explicit(&target_tw->canceled_queue.head, memory_order_relaxed);
    do {
        atomic_store_explicit(&timer->cancel_next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&target_tw->canceled_queue.head, &head, timer,
                                                    memory_order_release, memory_order_relaxed));

    if (head == NULL && target_tw->runtime) {
        xr_runtime_set_timer_pending(target_tw->runtime, target_tw->owner_worker_id, true);
        xr_runtime_wake_worker(target_tw->runtime, target_tw->owner_worker_id);
    }
}

// Process canceled stack (called by owner worker ONLY, before bump)
// Returns number of timers processed
int xr_timer_process_canceled_queue(XrTimerWheel *tw) {
    XR_DCHECK(tw != NULL, "timer_process_canceled_queue: NULL tw");
    // Owner-only assertion (skip during startup when TLS is unset).
    XrWorker *cur = xr_current_worker();
    XR_DCHECK(cur == NULL || cur->p.id == tw->owner_worker_id,
              "timer_process_canceled_queue: non-owner call");
    (void) cur;
    XrTimerCancelQueue *cq = &tw->canceled_queue;
    int count = 0;
    int drained = 0;

    XrTWheelTimer *timer = atomic_load_explicit(&cq->head, memory_order_acquire);
    if (!timer) {
        return 0;
    }

    timer = atomic_exchange_explicit(&cq->head, NULL, memory_order_acquire);
    while (timer) {
        XrTWheelTimer *next = atomic_load_explicit(&timer->cancel_next, memory_order_relaxed);
        atomic_store_explicit(&timer->cancel_next, NULL, memory_order_relaxed);

        if (timer &&
            atomic_load_explicit(&timer->state, memory_order_acquire) == XR_TIMER_STATE_ZOMBIE &&
            timer->owner_worker_id == tw->owner_worker_id) {
            if (timer->slot != XR_TW_SLOT_INACTIVE) {
                if (timer_is_in_slot(tw, timer, timer->slot)) {
                    remove_timer(tw, timer);
                    tw->nto--;
                    atomic_store_explicit(&timer->state, XR_TIMER_STATE_ACTIVE,
                                          memory_order_release);
                    count++;
                }
            } else {
                atomic_store_explicit(&timer->state, XR_TIMER_STATE_ACTIVE, memory_order_release);
            }
        }

        drained++;
        timer = next;
    }

    if (drained > 0 && tw->runtime) {
        xr_sched_metric_inc(tw->runtime, &tw->runtime->sched_stats.timer_cancel_drain_batch_count);
        xr_sched_metric_add(tw->runtime, &tw->runtime->sched_stats.timer_cancel_drain_node_count,
                            (uint64_t) drained);
        xr_sched_metric_max(tw->runtime, &tw->runtime->sched_stats.timer_cancel_drain_max_count,
                            (uint64_t) drained);
    }
    if (count > 0 && tw->runtime) {
        xr_sched_metric_add(tw->runtime, &tw->runtime->sched_stats.timer_cancel_process_count,
                            (uint64_t) count);
    }
    if (drained > 0) {
        timer_refresh_runtime_mask(tw);
    }
    return count;
}

// ========== Public API ==========

// Create timer wheel for a specific worker
XrTimerWheel *xr_timer_wheel_create(XrRuntime *runtime, int owner_worker_id) {
    XR_DCHECK(runtime != NULL, "timer_wheel_create: NULL runtime");
    XrTimerWheel *tw;
    int64_t mtime;
    int i;

    tw = (XrTimerWheel *) xr_calloc(1, sizeof(XrTimerWheel));
    if (!tw)
        return NULL;

    tw->w = &tw->slots[1];
    for (i = XR_TW_SLOT_AT_ONCE; i < XR_TW_LATER_WHEEL_END_SLOT; i++)
        tw->w[i] = NULL;

    for (i = 0; i < XR_TW_SCNT_SIZE; i++)
        tw->scnt[i] = 0;

    mtime = xr_monotonic_ticks();
    tw->pos = mtime;
    tw->nto = 0;
    tw->at_once.nto = 0;
    tw->soon.min_tpos = XR_TW_MAX_TICKS;
    tw->soon.nto = 0;
    tw->later.min_tpos = XR_TW_MAX_TICKS;
    tw->later.min_tpos_slot = XR_TW_LATER_WHEEL_END_SLOT;
    tw->later.pos = tw->pos + XR_TW_SOON_WHEEL_SIZE;
    tw->later.pos &= XR_TW_LATER_WHEEL_POS_MASK;
    tw->later.nto = 0;
    tw->yield_slot = XR_TW_SLOT_INACTIVE;
    tw->true_next_timeout_time = 0;
    xr_tw_set_next_pos(tw, tw->pos + XR_TW_TICKS_WEEK);
    tw->next_timeout_time = xr_tw_next_pos(tw);
    tw->sentinel.next = &tw->sentinel;
    tw->sentinel.prev = &tw->sentinel;
    atomic_store_explicit(&tw->sentinel.cancel_next, NULL, memory_order_relaxed);
    tw->sentinel.timeout = NULL;
    tw->sentinel.arg = NULL;
    // Record ownership and initialize the cancel stack.
    tw->owner_worker_id = owner_worker_id;
    tw->runtime = runtime;
    xr_timer_cancel_queue_init(&tw->canceled_queue);

    return tw;
}

// Destroy timer wheel
void xr_timer_wheel_destroy(XrTimerWheel *tw) {
    if (!tw)
        return;

    xr_free(tw);
}

// Set timer (MUST be called from owner worker only)
bool xr_twheel_set_timer(XrTimerWheel *tw, XrTWheelTimer *p, XrTimeoutProc timeout, void *arg,
                         int64_t timeout_pos) {
    XR_DCHECK(tw != NULL, "twheel_set_timer: NULL tw");
    XR_DCHECK(p != NULL, "twheel_set_timer: NULL timer");
    // Owner-only assertion (skip during startup when TLS is unset).
    XrWorker *cur_w = xr_current_worker();
    XR_DCHECK(cur_w == NULL || cur_w->p.id == tw->owner_worker_id,
              "twheel_set_timer: non-owner call");
    (void) cur_w;
    int slot;

    // No mutex needed - owner worker exclusive access

    p->timeout = timeout;
    p->arg = arg;

    if (atomic_load_explicit(&p->state, memory_order_acquire) == XR_TIMER_STATE_ZOMBIE) {
        xr_timer_process_canceled_queue(tw);
        if (atomic_load_explicit(&p->state, memory_order_acquire) == XR_TIMER_STATE_ZOMBIE) {
            if (p->owner_worker_id == tw->owner_worker_id && p->slot != XR_TW_SLOT_INACTIVE &&
                timer_is_in_slot(tw, p, p->slot)) {
                remove_timer(tw, p);
                tw->nto--;
                atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE, memory_order_release);
            } else if (p->slot == XR_TW_SLOT_INACTIVE) {
                atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE, memory_order_release);
            } else {
                return false;
            }
        }
    }

    XR_TW_ASSERT(p->slot == XR_TW_SLOT_INACTIVE);
    atomic_store_explicit(&p->cancel_next, NULL, memory_order_relaxed);
    p->owner_worker_id = tw->owner_worker_id;  // Record ownership

    tw->nto++;

    if (timeout_pos <= tw->pos) {
        p->timeout_pos = timeout_pos = tw->pos;
        slot = XR_TW_SLOT_AT_ONCE;
    } else if (timeout_pos < tw->pos + XR_TW_SOON_WHEEL_SIZE) {
        p->timeout_pos = timeout_pos;
        slot = soon_slot(timeout_pos);
        if (tw->soon.min_tpos > timeout_pos)
            tw->soon.min_tpos = timeout_pos;
    } else {
        p->timeout_pos = timeout_pos;
        slot = later_slot(timeout_pos);
        timeout_pos &= XR_TW_LATER_WHEEL_POS_MASK;
        timeout_pos -= XR_TW_LATER_WHEEL_SLOT_SIZE;
    }

    insert_timer_into_slot(tw, slot, p);

    if (timeout_pos <= xr_tw_next_pos(tw)) {
        tw->true_next_timeout_time = 1;
        if (timeout_pos < xr_tw_next_pos(tw)) {
            xr_tw_set_next_pos(tw, timeout_pos);
            tw->next_timeout_time = timeout_pos;
        }
    }
    timer_refresh_runtime_mask(tw);
    return true;
}

// Cancel timer (MUST be called from owner worker only)
// For cross-worker cancel, use xr_timer_queue_cancel() instead
void xr_twheel_cancel_timer(XrTimerWheel *tw, XrTWheelTimer *p) {
    XR_DCHECK(tw != NULL, "twheel_cancel_timer: NULL tw");
    XR_DCHECK(p != NULL, "twheel_cancel_timer: NULL timer");
    // Owner-only assertion (skip during startup when TLS is unset).
    XrWorker *cur_w = xr_current_worker();
    XR_DCHECK(cur_w == NULL || cur_w->p.id == tw->owner_worker_id,
              "twheel_cancel_timer: non-owner call");
    (void) cur_w;
    if (tw->runtime) {
        xr_sched_metric_inc(tw->runtime, &tw->runtime->sched_stats.timer_cancel_local_count);
    }
    // No mutex needed - owner worker exclusive access
    if (p->owner_worker_id != tw->owner_worker_id)
        return;
    if (p->slot != XR_TW_SLOT_INACTIVE) {
        if (!timer_is_in_slot(tw, p, p->slot)) {
            atomic_store_explicit(&p->state, XR_TIMER_STATE_ZOMBIE, memory_order_release);
            timer_refresh_runtime_mask(tw);
            return;
        }
        remove_timer(tw, p);
        tw->nto--;
        atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE, memory_order_release);
    }
    timer_refresh_runtime_mask(tw);
}

// Advance timer wheel (owner worker only, no cross-thread access)
void xr_bump_timers(XrTimerWheel *tw, int64_t curr_time) {
    XR_DCHECK(tw != NULL, "bump_timers: NULL tw");
    int slot, restarted, yield_count, slots, scnt_ix;
    int64_t bump_to;
    int64_t *scnt, *bump_scnt;

    // No mutex needed - owner worker exclusive access

    // process cross-worker cancellation queue first
    xr_timer_process_canceled_queue(tw);

    yield_count = XR_TW_BUMP_YIELD_LIMIT;
    scnt = &tw->scnt[0];
    bump_scnt = &tw->bump_scnt[0];

    slot = tw->yield_slot;
    restarted = slot != XR_TW_SLOT_INACTIVE;
    if (restarted) {
        bump_to = tw->pos;
        if (slot >= XR_TW_LATER_WHEEL_FIRST_SLOT)
            goto restart_yielded_later_slot;
        tw->yield_slot = XR_TW_SLOT_INACTIVE;
        if (slot == XR_TW_SLOT_AT_ONCE)
            goto restart_yielded_at_once_slot;
        scnt_ix = scnt_get_ix(slot);
        slots = tw->yield_slots_left;
        goto restart_yielded_soon_slot;
    }

    do {
        restarted = 0;
        bump_to = curr_time;
        tw->true_next_timeout_time = 1;
        xr_tw_set_next_pos(tw, bump_to);
        tw->next_timeout_time = bump_to;

        while (1) {
            XrTWheelTimer *p;

            if (tw->nto == 0) {
            empty_wheel:
                tw->true_next_timeout_time = 0;
                xr_tw_set_next_pos(tw, bump_to + XR_TW_TICKS_WEEK);
                tw->next_timeout_time = xr_tw_next_pos(tw);
                tw->pos = bump_to;
                tw->later.pos = bump_to + XR_TW_SOON_WHEEL_SIZE;
                tw->later.pos &= XR_TW_LATER_WHEEL_POS_MASK;
                tw->yield_slot = XR_TW_SLOT_INACTIVE;
                timer_refresh_runtime_mask(tw);
                return;
            }

            p = tw->w[XR_TW_SLOT_AT_ONCE];

            if (p) {
                if (p->next == p) {
                    XR_TW_ASSERT(tw->sentinel.next == &tw->sentinel);
                    XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
                } else {
                    tw->sentinel.next = p->next;
                    tw->sentinel.prev = p->prev;
                    tw->sentinel.next->prev = &tw->sentinel;
                    tw->sentinel.prev->next = &tw->sentinel;
                }
                tw->w[XR_TW_SLOT_AT_ONCE] = NULL;

                while (1) {
                    XR_TW_ASSERT(tw->nto > 0);
                    XR_TW_ASSERT(tw->at_once.nto > 0);
                    tw->nto--;
                    tw->at_once.nto--;

                    // Set slot = INACTIVE immediately, allow safe external reuse
                    p->slot = XR_TW_SLOT_INACTIVE;

                    // skip zombie timers (cancelled during bump)
                    if (atomic_load_explicit(&p->state, memory_order_acquire) !=
                        XR_TIMER_STATE_ZOMBIE) {
                        timeout_timer(tw, p);
                        yield_count -= XR_TW_COST_TIMEOUT;
                    }
                    // Reset state for timer reuse
                    atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE, memory_order_release);

                restart_yielded_at_once_slot:

                    p = tw->sentinel.next;
                    if (p == &tw->sentinel) {
                        XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
                        break;
                    }

                    if (yield_count <= 0) {
                        XR_TW_ASSERT(tw->nto > 0);
                        XR_TW_ASSERT(tw->at_once.nto > 0);
                        tw->yield_slot = XR_TW_SLOT_AT_ONCE;
                        timer_refresh_runtime_mask(tw);
                        return;
                    }

                    tw->sentinel.next = p->next;
                    p->next->prev = &tw->sentinel;
                }
            }

            if (tw->pos >= bump_to) {
                if (tw->at_once.nto)
                    continue;
                break;
            }

            if (tw->nto == 0)
                goto empty_wheel;

            memcpy((void *) bump_scnt, (void *) scnt, sizeof(int64_t) * XR_TW_SCNT_SIZE);

            if (tw->soon.min_tpos > tw->pos) {
                int64_t skip_until_pos = tw->soon.min_tpos;

                if (skip_until_pos > bump_to)
                    skip_until_pos = bump_to;

                skip_until_pos--;

                if (skip_until_pos > tw->pos)
                    tw->pos = skip_until_pos;
            }

            {
                int64_t tmp_slots = bump_to - tw->pos;
                if (tmp_slots < XR_TW_SOON_WHEEL_SIZE)
                    slots = (int) tmp_slots;
                else
                    slots = XR_TW_SOON_WHEEL_SIZE;
            }

            slot = soon_slot(tw->pos + 1);
            tw->pos = bump_to;

            xr_tw_set_next_pos(tw, bump_to);
            tw->next_timeout_time = bump_to;

            scnt_ix = scnt_get_ix(slot);

            while (slots > 0) {
                yield_count -= XR_TW_COST_SLOT;

                p = tw->w[slot];
                if (p) {
                    if (p->next == p) {
                        XR_TW_ASSERT(tw->sentinel.next == &tw->sentinel);
                        XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
                    } else {
                        tw->sentinel.next = p->next;
                        tw->sentinel.prev = p->prev;
                        tw->sentinel.next->prev = &tw->sentinel;
                        tw->sentinel.prev->next = &tw->sentinel;
                    }
                    tw->w[slot] = NULL;

                    while (1) {
                        // Timer may have been cancelled or reused during yield, skip if slot
                        // doesn't match
                        if (p->slot != slot) {
                            goto restart_yielded_soon_slot;
                        }
                        if (--tw->soon.nto == 0)
                            tw->soon.min_tpos = XR_TW_MAX_TICKS;
                        scnt_ix_dec(scnt, scnt_ix);

                        // Set slot = INACTIVE immediately, allow safe external reuse
                        p->slot = XR_TW_SLOT_INACTIVE;

                        if (p->timeout_pos <= bump_to) {
                            // skip zombie timers (cancelled during bump)
                            if (atomic_load_explicit(&p->state, memory_order_acquire) !=
                                XR_TIMER_STATE_ZOMBIE) {
                                timeout_timer(tw, p);
                                yield_count -= XR_TW_COST_TIMEOUT;
                            }
                            // Reset state for timer reuse
                            atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE,
                                                  memory_order_release);
                            tw->nto--;
                            scnt_ix_dec(bump_scnt, scnt_ix);
                        } else {
                            // Timer not yet due, re-insert (may need to check zombie too)
                            if (atomic_load_explicit(&p->state, memory_order_acquire) !=
                                XR_TIMER_STATE_ZOMBIE) {
                                insert_timer_into_slot(tw, slot, p);
                                yield_count -= XR_TW_COST_SLOT_MOVE;
                            } else {
                                // Zombie timer, just clean up
                                atomic_store_explicit(&p->state, XR_TIMER_STATE_ACTIVE,
                                                      memory_order_release);
                                tw->nto--;
                                scnt_ix_dec(bump_scnt, scnt_ix);
                            }
                        }

                    restart_yielded_soon_slot:

                        p = tw->sentinel.next;
                        if (p == &tw->sentinel) {
                            XR_TW_ASSERT(tw->sentinel.prev == &tw->sentinel);
                            break;
                        }

                        if (yield_count <= 0) {
                            tw->yield_slot = slot;
                            tw->yield_slots_left = slots;
                            timer_refresh_runtime_mask(tw);
                            return;
                        }

                        tw->sentinel.next = p->next;
                        p->next->prev = &tw->sentinel;
                    }
                }

                scnt_soon_wheel_next(&slot, &slots, NULL, &scnt_ix, bump_scnt);
            }

            if (XR_TW_BUMP_LATER_WHEEL(tw)) {
            restart_yielded_later_slot:
                if (bump_later_wheel(tw, &yield_count)) {
                    timer_refresh_runtime_mask(tw);
                    return;
                }
            }
        }

    } while (restarted);

    tw->true_next_timeout_time = 0;
    XR_TW_ASSERT(xr_tw_next_pos(tw) == bump_to);

    (void) find_next_timeout(tw);
    timer_refresh_runtime_mask(tw);
}

// Check next timeout time (called by owner worker only - )
int64_t xr_check_next_timeout_time(XrTimerWheel *tw) {
    XR_DCHECK(tw != NULL, "check_next_timeout_time: NULL tw");
    int64_t time;

    // No mutex needed - owner worker exclusive access

    XR_TW_ASSERT(tw->next_timeout_time == xr_tw_next_pos(tw));

    if (tw->true_next_timeout_time) {
        time = tw->next_timeout_time;
    } else if (xr_tw_next_pos(tw) > tw->pos + XR_TW_SOON_WHEEL_SIZE) {
        time = tw->next_timeout_time;
    } else {
        time = find_next_timeout(tw);
    }

    return time;
}

// Get next timeout reference
int64_t *xr_get_next_timeout_reference(XrTimerWheel *tw) {
    XR_DCHECK(tw != NULL, "get_next_timeout_reference: NULL tw");
    return &tw->next_timeout_time;
}
