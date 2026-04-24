/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_sysmon.c - System monitor, main thread loop, and debug support
 *
 * KEY CONCEPT:
 *   Sysmon thread monitors worker health (preemption, GC scheduling).
 *   Main thread run loop handles the initial coroutine.
 *   Debug resume and select block/unblock support.
 */

#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#ifdef XRAY_HAS_JIT
#include "../jit/xir_jit_debug.h"
#endif

/* Arm guard pages for all running workers so JIT back-edges trigger safepoints.
 * Called by sysmon every ~2ms. Workers that are parked or in syscall are skipped. */
static void sysmon_arm_guard_pages(XrRuntime *runtime) {
#ifdef XRAY_HAS_JIT
    for (int i = 0; i < runtime->worker_count; i++) {
        XrWorker *w = &runtime->workers[i];
        XrMachine *wm = w->m;
        if (!wm) continue;

        int st = atomic_load_explicit(&wm->state, memory_order_relaxed);
        if (st != M_RUNNING) continue;

        void *page = w->p.jit_scratch.safepoint_page;
        if (page) jit_guard_page_arm(page);
    }
#else
    (void)runtime;
#endif
}

static void sysmon_check(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "sysmon_check: NULL runtime");
    int64_t now_us = get_current_time_us();

    for (int i = 0; i < runtime->worker_count; i++) {
        XrWorker *w = &runtime->workers[i];
        XrMachine *wm = w->m;

        // Skip workers with no bound M (M is blocked in syscall, handoff in progress)
        if (!wm) {
            runtime->sysmon_state[i].stuck_since_us = 0;
            runtime->sysmon_state[i].warned = false;
            continue;
        }

        uint64_t hb = atomic_load_explicit(&wm->heartbeat, memory_order_relaxed);

        // P_SYSCALL/P_HANDOFF: M is in syscall or handoff M is running
        uint32_t pstatus = atomic_load_explicit(&w->p.status, memory_order_relaxed);
        if (pstatus == P_SYSCALL || pstatus == P_HANDOFF) {
            runtime->sysmon_state[i].last_heartbeat = hb;
            runtime->sysmon_state[i].stuck_since_us = 0;
            runtime->sysmon_state[i].warned = false;
            continue;
        }

        if (hb == runtime->sysmon_state[i].last_heartbeat && hb != 0) {
            // Only track stall when Worker is actively running a coroutine.
            // Parked/idle Workers naturally have stalled heartbeats.
            int st = atomic_load_explicit(&wm->state, memory_order_relaxed);
            if (st != M_RUNNING) {
                runtime->sysmon_state[i].stuck_since_us = 0;
                runtime->sysmon_state[i].warned = false;
                runtime->sysmon_state[i].last_heartbeat = hb;
                continue;
            }

            // heartbeat unchanged while RUNNING → record stuck start
            if (runtime->sysmon_state[i].stuck_since_us == 0) {
                runtime->sysmon_state[i].stuck_since_us = now_us;
                runtime->sysmon_state[i].warned = false;

                // Immediate rescue steal on first detection (O(1))
                int queued = xr_worker_total_queue_len(w);
                if (queued > 0) {
                    wake_idle_worker(runtime);
                }
            }

            int64_t elapsed_us = now_us - runtime->sysmon_state[i].stuck_since_us;

            // === Level 1.5 (10ms): Auto-upgrade FAST C function to SLOW ===
            // If a C function marked FAST actually blocks >10ms, increment
            // its auto_slow_count. After 3 occurrences, upgrade to SLOW so
            // future calls release P before execution (dirty worker path).
            // Phase 2 (CORO-04): all accesses are atomic; upgrade uses
            // one-way CAS to prevent ABA with concurrent VM reads.
            if (elapsed_us >= 10000 && wm->current_cfunc) {
                XrCFunction *cfn = (XrCFunction *)wm->current_cfunc;
                uint8_t cls = atomic_load_explicit(&cfn->cfunc_class,
                                                    memory_order_relaxed);
                if (cls == XR_CFUNC_FAST) {
                    uint8_t cnt = atomic_fetch_add_explicit(
                        &cfn->auto_slow_count, 1, memory_order_relaxed) + 1;
                    if (cnt >= 3) {
                        // One-way CAS: FAST → SLOW (never reverts)
                        uint8_t expected = XR_CFUNC_FAST;
                        atomic_compare_exchange_strong_explicit(
                            &cfn->cfunc_class, &expected, XR_CFUNC_SLOW,
                            memory_order_release, memory_order_relaxed);
                    }
                }
            }

            // === Level 2 (100ms): warning log (once) ===
            if (!runtime->sysmon_state[i].warned && elapsed_us >= XR_SYSMON_WARN_US) {
                runtime->sysmon_state[i].warned = true;
                XrCoroutine *coro = wm->current_coro;
                xr_log_warning("sysmon", "Worker %d stuck for %lldms"
                                " (coroutine: %s, hb=%llu)",
                        i, (long long)(elapsed_us / 1000),
                        (coro && coro->name) ? coro->name : "unknown",
                        (unsigned long long)hb);
            }

            // === Level 3 (5s): mark coroutine for cancellation ===
            if (elapsed_us >= XR_SYSMON_CANCEL_US) {
                XrCoroutine *coro = wm->current_coro;
                if (coro && !xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                    xr_coro_flags_set(coro, XR_CORO_FLG_CANCEL_REQUESTED);
                    // Diagnostic: read from vm_ctx (active during execution)
                    const char *func_name = "?";
                    int in_c = 0;
                    int fc = coro->vm_ctx.frame_count;
                    XrBcCallFrame *frames = coro->vm_ctx.frames;
                    if (fc > 0 && frames) {
                        XrBcCallFrame *f = &frames[fc - 1];
                        in_c = (f->call_status & XR_CALL_C) ? 1 : 0;
                        if (f->closure && f->closure->proto && f->closure->proto->name) {
                            func_name = f->closure->proto->name->data;
                        }
                    }
                    const char *entry_str = "closure";
                    if (coro->entry_type == XR_CORO_ENTRY_NATIVE) entry_str = "native";
                    else if (coro->entry_type == XR_CORO_ENTRY_CFUNC) entry_str = "cfunc";
                    xr_log_warning("sysmon", "Worker %d stuck for %lldms, "
                                    "coro id=%d '%s' cancelled "
                                    "(entry=%s, frame: %s, c=%d, fc=%d, reds=%d, flags=0x%x)",
                            i, (long long)(elapsed_us / 1000),
                            coro->id, coro->name ? coro->name : "unknown",
                            entry_str, func_name, in_c, fc,
                            coro->reductions, xr_coro_flags_load(coro));
                }
            }
        } else {
            // heartbeat progressing → normal
            runtime->sysmon_state[i].stuck_since_us = 0;
            runtime->sysmon_state[i].warned = false;
        }
        runtime->sysmon_state[i].last_heartbeat = hb;
    }
}

// Sysmon assist: wake parked workers that have expired timers.
//
// Timer callbacks use xr_current_worker() (TLS) which is NULL on the
// sysmon thread, so sysmon must NOT call xr_bump_timers directly.
// Instead, unpark the worker and let it bump its own timer wheel
// inside worker_poll_sources where TLS is valid.
static void sysmon_assist(XrRuntime *runtime) {
    int64_t now = xr_monotonic_ticks();

    for (int i = 0; i < runtime->worker_count; i++) {
        XrWorker *w = &runtime->workers[i];
        XrMachine *wm = w->m;
        if (!wm) continue;

        int st = atomic_load_explicit(&wm->state, memory_order_relaxed);
        if (st != M_PARKED && st != M_PARKING) continue;

        // If this worker has a timer wheel with overdue ticks, wake it
        if (w->p.timer_wheel && now > w->p.last_timer_tick) {
            int64_t next = xr_check_next_timeout_time(w->p.timer_wheel);
            if (next <= now) {
                worker_unpark(w);
            }
        }
    }
}

// Sysmon thread: heartbeat monitoring + stuck Worker detection + assist.
// Adaptive interval: busy system checks more frequently.
void *sysmon_thread_func(void *arg) {
    XrRuntime *runtime = (XrRuntime *)arg;
    int idle_rounds = 0;

    while (atomic_load(&runtime->running)) {
        // Adaptive interval: 2ms when active workers exist, up to 10ms when all idle
        int active = atomic_load_explicit(&runtime->active_workers, memory_order_relaxed);
        int64_t interval_ns;
        if (active > 0) {
            interval_ns = 2000000;  // 2ms (~500Hz) when busy
            idle_rounds = 0;
        } else {
            idle_rounds++;
            // Exponential backoff: 2ms → 5ms → 10ms
            if (idle_rounds < 5)
                interval_ns = 5000000;   // 5ms
            else
                interval_ns = 10000000;  // 10ms
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = interval_ns };
        nanosleep(&ts, NULL);
        sysmon_check(runtime);
        sysmon_assist(runtime);
        sysmon_arm_guard_pages(runtime);
    }
    return NULL;
}

// xr_runtime_next_coro_id - Allocate coroutine ID
int xr_runtime_next_coro_id(XrRuntime *runtime) {
    if (!runtime) return 0;
    return atomic_fetch_add(&runtime->next_coro_id, 1);
}

// xr_main_thread_run - Unified scheduling entry
//
// Core design: main thread directly calls worker_loop, fully reuses unified scheduling logic
//
// Design points:
// 1. Main thread reuses Worker 0
// 2. main coroutine put into global queue
// 3. Directly call worker_loop, no code duplication
// 4. When main coroutine completes, worker_loop internally sets runtime->running = false
int xr_main_thread_run(XrayIsolate *X, XrCoroutine *main_coro) {
    if (!X || !main_coro) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return -1;

    // Main thread reuses Worker 0
    XrWorker *worker = &runtime->workers[0];

    // Re-entry support (REPL): if runtime was stopped by previous main coro completion,
    // rejoin dead threads and restart state for next run.
    if (!atomic_load(&runtime->running)) {
        // Only join threads if they were started in previous run
        if (atomic_load(&runtime->threads_started)) {
            pthread_join(runtime->sysmon_thread, NULL);
            for (int i = 1; i < runtime->worker_count; i++) {
                XrMachine *wm = runtime->workers[i].m;
                if (wm) pthread_join(wm->thread, NULL);
            }
        }

        // Reset state for next run (threads created lazily on next spawn)
        atomic_store(&runtime->started_workers, 0);
        atomic_store(&runtime->exited_workers, 0);
        atomic_store(&runtime->threads_started, false);
        atomic_store(&runtime->running, true);
    }

 // Initialize main thread Worker's Timer Wheel (owner is worker 0)
    if (!worker->p.timer_wheel) {
        worker->p.timer_wheel = xr_timer_wheel_create(runtime, 0);  // Worker 0 owns this
        worker->p.last_timer_tick = xr_monotonic_ticks();
    }

    // Ensure vm_ctx.isolate is set correctly
    worker->m->vm_ctx.isolate = X;

    // put main coroutine into Worker 0's local queue (no global queue)
    // Clear stale execution state from previous run (DONE, RUNNING, BLOCKED, etc.)
    // xr_coro_flags_set is atomic OR — without clearing first, old DONE flag persists
    // and worker_loop skips the coroutine (line 1263: "prevent re-executing completed coroutine").
    xr_coro_flags_clear(main_coro,
        XR_CORO_FLG_DONE | XR_CORO_FLG_RUNNING | XR_CORO_FLG_BLOCKED |
        XR_CORO_FLG_CANCELLED | XR_CORO_FLG_IN_RUNQ | XR_CORO_FLG_STARTED);
    xr_coro_flags_set(main_coro, XR_CORO_FLG_READY | XR_CORO_FLG_MAIN);
    atomic_store_explicit(&main_coro->affinity_p, 0, memory_order_relaxed);  // main coroutine bound to Worker 0
    xr_worker_push(worker, main_coro);

    // Directly call worker_loop, reuse unified scheduling logic
    // worker_loop internally detects main coroutine completion and sets runtime->running = false
    worker_loop(worker);

    // Wake all sleeping Workers so they can observe running=false and exit
    for (int i = 0; i < runtime->worker_count; i++) {
        worker_unpark(&runtime->workers[i]);
    }

    // Wake idle M threads parked in handoff_thread_entry.
    //
    // Lock-free traversal (Phase 4.1): idle_m_head is a Treiber stack. We
    // snapshot the head and walk forward; any M pushed concurrently will
    // observe runtime->running=false on its next check anyway.
    {
        XrMachine *idle = atomic_load_explicit(&runtime->idle_m_head, memory_order_acquire);
        while (idle) {
            atomic_store_explicit(&idle->park_state, XR_PARK_WOKEN, memory_order_release);
            xr_park_futex_wake(&idle->park_state);
            idle = idle->idle_link;
        }
    }

    // Join worker threads before returning — prevents use-after-free when
    // caller frees protos while workers still execute coroutine bytecode.
    if (atomic_load(&runtime->threads_started)) {
        pthread_join(runtime->sysmon_thread, NULL);
        for (int i = 1; i < runtime->worker_count; i++) {
            XrMachine *wm = runtime->workers[i].m;
            if (wm) pthread_join(wm->thread, NULL);
        }
        // Mark threads as joined so next run doesn't double-join
        atomic_store(&runtime->threads_started, false);
    }

    // Cleanup TLS
    tls_current_worker = NULL;
    tls_current_machine = NULL;

    return 0;
}

// xr_debug_resume_vm - Resume VM execution after debug break
//
// Called by DAP server to resume execution after hitting a breakpoint.
// Returns 0 if stopped again (breakpoint/step), 1 if program ended.
int xr_debug_resume_vm(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return -1;

    XrWorker *worker = &runtime->workers[0];

    // Restart runtime
    atomic_store(&runtime->running, true);

    // Ensure TLS is set
    tls_current_worker = worker;

    // Set resume status to indicate debug resumption (skip unroll)
    xr_coro_resume_store(coro, XR_RESUME_DEBUG);

    // Use standard coroutine run mechanism with debug flag
    XrVMResult result = xr_coro_run_on_worker(worker, coro);

    atomic_store(&runtime->running, false);

    switch (result) {
        case XR_VM_OK:
            return 1;  // Program ended

        case XR_VM_DEBUG_BREAK:
            return 0;  // Stopped at breakpoint/step

        case XR_VM_YIELD:
        case XR_VM_BLOCKED:
            return 0;  // Treat as stopped for debugging

        default:
            return 1;  // Program ended (with error)
    }
}

// ========== Select Event-driven Support (lock-free) ==========

// xr_worker_block_select - Add coroutine to multiple Channel wait queues in select mode
//
// Phase 6.1 redesign: each XrSelectCase embeds a bucket_next pointer, so the
// case is linked into the corresponding bucket's select queue directly.
// wake_select then traverses only the target bucket — O(waiters_on_channel).
void xr_worker_block_select(XrWorker *worker, XrCoroutine *coro,
                            void **channels, int count) {
    if (!worker || !coro || !channels || count <= 0) return;

    XrSelectWait *sw = coro->select_wait;
    XR_DCHECK(sw != NULL, "block_select: coro has no select_wait");

    // Link each case into its channel's bucket select queue.
    // Also set waiter_worker_mask on each channel for wake routing.
    for (int i = 0; i < count; i++) {
        void *channel = channels[i];
        if (!channel) continue;

        XrBlockedBucket *bucket = worker_blocked_bucket_find_or_create(worker, channel);
        if (!bucket) continue;

        XR_DCHECK(i < sw->case_count, "block_select: case index out of bounds");
        XrSelectCase *sc = &sw->cases[i];
        sc->bucket_next = NULL;
        if (bucket->select_tail) {
            bucket->select_tail->bucket_next = sc;
        } else {
            bucket->select_head = sc;
        }
        bucket->select_tail = sc;

        // Set waiter mask bit for this worker on the channel
        XrChannel *ch = (XrChannel *)channel;
        atomic_fetch_or_explicit(&ch->waiter_worker_mask,
                                 (uint64_t)1 << worker->p.id, memory_order_relaxed);
    }

    // Add to Worker's linear blocked queue (for sysmon / timer traversal)
    worker_blocked_list_add(worker, coro);
    worker->p.blocked_count++;
    worker->p.select_waiter_count++;

    // Set coroutine state
    xr_coro_flags_clear(coro, XR_CORO_FLG_READY);
    xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_SELECT);
}

// xr_worker_wake_select - Wake select coroutine waiting on specified Channel
//
// Phase 6.1: traverse bucket->select_head (XrSelectCase chain) instead of
// scanning all blocked coros.  Complexity = O(select waiters on this channel).
XrCoroutine *xr_worker_wake_select(XrWorker *worker, void *channel) {
    if (!worker || !channel) return NULL;
    // Phase 0: MUST only be called from the owning worker thread.
    XR_DCHECK(xr_current_worker() == worker,
              "wake_select: cross-worker call detected (use chan_wake_queue)");

    // Fast path: no select waiters at all
    if (worker->p.select_waiter_count == 0) return NULL;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket || !bucket->select_head) return NULL;

    // Walk the bucket's select case chain
    XrSelectCase *sc = bucket->select_head;
    while (sc) {
        XrCoroutine *coro = sc->owner;
        XR_DCHECK(coro != NULL, "wake_select: NULL owner on select case");
        XrSelectWait *sw = coro->select_wait;

        if (sw && !atomic_load(&sw->triggered)) {
            // CAS to prevent duplicate wake from another channel
            bool expected = false;
            if (atomic_compare_exchange_strong(&sw->triggered, &expected, true)) {
                // Determine case index from pointer arithmetic
                int case_idx = (int)(sc - sw->cases);
                XR_DCHECK(case_idx >= 0 && case_idx < sw->case_count,
                          "wake_select: case index out of range");
                coro->select_ready_case = case_idx;

                // Remove from all bucket queues + blocked list
                xr_worker_unblock_select(worker, coro);

                // Set wake state
                xr_coro_resume_store(coro, XR_RESUME_CHANNEL);
                xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
                xr_coro_flags_set(coro, XR_CORO_FLG_READY);

                xr_worker_push(worker, coro);
                return coro;
            }
        }
        sc = sc->bucket_next;
    }

    return NULL;
}

// Remove a single XrSelectCase from its bucket's select queue.
static void select_case_remove_from_bucket(XrWorker *worker, XrSelectCase *target) {
    if (!target->channel) return;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, target->channel);
    if (!bucket) return;

    XrSelectCase *prev = NULL;
    XrSelectCase *curr = bucket->select_head;
    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->bucket_next = curr->bucket_next;
            } else {
                bucket->select_head = curr->bucket_next;
            }
            if (bucket->select_tail == curr) {
                bucket->select_tail = prev;
            }
            curr->bucket_next = NULL;
            return;
        }
        prev = curr;
        curr = curr->bucket_next;
    }
}

// xr_worker_unblock_select - Remove select coroutine from ALL Channel wait queues
//
// Phase 6.1: iterate every case in the select and remove its node from the
// corresponding bucket, instead of only the first channel.
void xr_worker_unblock_select(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

    XrSelectWait *sw = coro->select_wait;
    if (!sw) return;

    // Notify dist channels about exiting select (unsubscribe from push model)
    XrChannelDistHooks *dhooks = coro->isolate ? coro->isolate->channel_dist_hooks : NULL;
    if (dhooks && dhooks->on_select_exit) {
        for (int i = 0; i < sw->case_count; i++) {
            XrChannel *ch = (XrChannel *)sw->cases[i].channel;
            if (ch && ch->dist) {
                dhooks->on_select_exit(ch);
            }
        }
    }

    // Remove each case from its channel's bucket select queue
    for (int i = 0; i < sw->case_count; i++) {
        select_case_remove_from_bucket(worker, &sw->cases[i]);
    }

    // Remove from Worker's linear blocked queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;
    worker->p.select_waiter_count--;

    // Cleanup list pointers
    coro->next = NULL;
    coro->prev = NULL;
}
