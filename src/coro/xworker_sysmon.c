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
            if (elapsed_us >= 10000 && wm->current_cfunc) {
                XrCFunction *cfn = (XrCFunction *)wm->current_cfunc;
                if (cfn->cfunc_class == XR_CFUNC_FAST) {
                    cfn->auto_slow_count++;
                    if (cfn->auto_slow_count >= 3) {
                        cfn->cfunc_class = XR_CFUNC_SLOW;
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

    // Wake idle M threads parked in handoff_thread_entry
    {
        pthread_mutex_lock(&runtime->sched_lock);
        XrMachine *idle = runtime->idle_m_head;
        while (idle) {
            atomic_store_explicit(&idle->park_state, XR_PARK_WOKEN, memory_order_release);
            xr_park_futex_wake(&idle->park_state);
            idle = idle->idle_link;
        }
        pthread_mutex_unlock(&runtime->sched_lock);
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
// Design points:
// 1. Coroutine joins each channel's select_head/select_tail queue
// 2. Use sched_link as select queue list pointer (not scheduled during select)
// 3. select_wait structure records all waiting channels, used for cleanup after wake
void xr_worker_block_select(XrWorker *worker, XrCoroutine *coro,
                            void **channels, int count) {
    if (!worker || !coro || !channels || count <= 0) return;
    
    // Add coroutine to each channel's select queue
    for (int i = 0; i < count; i++) {
        void *channel = channels[i];
        if (!channel) continue;
        
        XrBlockedBucket *bucket = worker_blocked_bucket_find_or_create(worker, channel);
        if (!bucket) continue;
        
        // Add to select queue tail (use next pointer, as only in one queue)
        // Note: since coroutine may be in multiple bucket's select queues,
        // we use linear traversal, don't directly use list pointer
        if (i == 0) {
            // Only actually link in first channel's queue
            coro->next = NULL;
            if (bucket->select_tail) {
                bucket->select_tail->next = coro;
            } else {
                bucket->select_head = coro;
            }
            bucket->select_tail = coro;
        }
    }
    
    // Add to Worker's linear blocked queue (for traversal)
    worker_blocked_list_add(worker, coro);
    worker->p.blocked_count++;
    worker->p.select_waiter_count++;  // Increment select waiter count
    
    // Set coroutine state
    xr_coro_flags_clear(coro, XR_CORO_FLG_READY);
    xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_SELECT);
}

// xr_worker_wake_select - Wake select coroutine waiting on specified Channel
//
// Design points:
// 1. Traverse Worker's blocked_head linear list
// 2. Check if each blocked coroutine is select-waiting on this channel
// 3. Use CAS to set triggered to prevent duplicate wake
// 4. Return the woken coroutine
XrCoroutine *xr_worker_wake_select(XrWorker *worker, void *channel) {
    if (!worker || !channel) return NULL;
    
    // Fast path: no select waiters, skip traversal
    if (worker->p.select_waiter_count == 0) return NULL;
    
    // Traverse all blocked coroutines in Worker
    XrCoroutine *coro = worker->p.blocked_head;
    while (coro) {
        XrCoroutine *next = coro->next;
        
        // Check if select waiting
        XrSelectWait *sw = coro->select_wait;
        if (sw && !atomic_load(&sw->triggered)) {
            // Check if waiting on this channel
            for (int i = 0; i < sw->case_count; i++) {
                if (sw->cases[i].channel == channel) {
                    // CAS set triggered to prevent duplicate wake
                    bool expected = false;
                    if (atomic_compare_exchange_strong(&sw->triggered, &expected, true)) {
                        // Record ready case index
                        coro->select_ready_case = i;
                        
                        // Remove from blocked queue
                        xr_worker_unblock_select(worker, coro);
                        
                        // Set wake state
                        xr_coro_resume_store(coro, XR_RESUME_CHANNEL);
                        xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
                        xr_coro_flags_set(coro, XR_CORO_FLG_READY);
                        
                        // Add to ready queue
                        xr_worker_push(worker, coro);
                        
                        return coro;
                    }
                    break;  // Already woken by another channel
                }
            }
        }
        
        coro = next;
    }
    
    return NULL;
}

// xr_worker_unblock_select - Remove select coroutine from all Channel wait queues
//
// Design points:
// 1. Remove from Worker's linear blocked queue
// 2. Remove from first channel's select queue (other channels checked via traversal)
// 3. Cleanup select_wait structure
void xr_worker_unblock_select(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;
    
    XrSelectWait *sw = coro->select_wait;
    if (!sw) return;
    
    // Notify dist channels about exiting select (unsubscribe from push model)
    XrChannelDistHooks *dhooks = xr_channel_dist_hooks;
    if (dhooks && dhooks->on_select_exit) {
        for (int i = 0; i < sw->case_count; i++) {
            XrChannel *ch = (XrChannel *)sw->cases[i].channel;
            if (ch && ch->dist) {
                dhooks->on_select_exit(ch);
            }
        }
    }
    
    // Remove from first channel's select queue
    if (sw->case_count > 0 && sw->cases[0].channel) {
        void *channel = sw->cases[0].channel;
        XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
        if (bucket) {
            // Remove from select queue (linear search)
            XrCoroutine *prev = NULL;
            XrCoroutine *curr = bucket->select_head;
            while (curr) {
                if (curr == coro) {
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        bucket->select_head = curr->next;
                    }
                    if (bucket->select_tail == curr) {
                        bucket->select_tail = prev;
                    }
                    break;
                }
                prev = curr;
                curr = curr->next;
            }
        }
    }
    
    // Remove from Worker's linear blocked queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;
    worker->p.select_waiter_count--;  // Decrement select waiter count
    
    // Cleanup list pointers
    coro->next = NULL;
    coro->prev = NULL;
}
