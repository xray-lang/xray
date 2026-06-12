/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scheduler_runq.c - Unit tests for the single scheduler run queue
 */

#include "../test_framework.h"
#include "base/xconstants.h"
#include "coro/xcoro_tuning.h"
#include "coro/xscheduler_policy.h"
#include "coro/xcoro_abi.h"
#include "coro/xtask.h"
#include "coro/xworker_internal.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct SchedulerFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker worker;
    XrMachine machine;
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool worker_initialized;
    bool inject_initialized;
    bool task_lock_initialized;
} SchedulerFixture;

static void fixture_init_runtime(XrRuntime *runtime, XrayIsolate *isolate, XrWorker *workers,
                                 XrMachine *machines, int worker_count) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->isolate = isolate;
    runtime->worker_count = worker_count;
    runtime->workers = workers;
    runtime->machines = machines;
    atomic_store(&runtime->running, true);
    atomic_store(&runtime->threads_started, false);
    atomic_store(&runtime->spinning_count, 0);
    atomic_store(&runtime->idle_worker_count, 0);
    atomic_store(&runtime->timer_p_mask, 0);
    atomic_store(&runtime->idle_p_mask, 0);
    atomic_store(&runtime->searching_count, 0);
    xr_mutex_init(&runtime->task_lock);
    runtime->task_list = NULL;
    runtime->task_count = 0;
}

static bool scheduler_fixture_init(SchedulerFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    fixture_init_runtime(&f->runtime, &f->isolate_storage, &f->worker, &f->machine, 1);
    xr_injectq_init(&f->runtime);
    f->inject_initialized = true;
    f->task_lock_initialized = true;

    xr_worker_init(&f->worker, 0, &f->runtime);
    f->worker_initialized = true;

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = &f->worker;
    tls_current_machine = &f->machine;
    return true;
}

static void scheduler_fixture_cleanup(SchedulerFixture *f) {
    xr_runtime_stop(&f->runtime);
    if (f->worker_initialized) {
        xr_worker_destroy(&f->worker);
        f->worker_initialized = false;
    }
    if (f->inject_initialized) {
        xr_injectq_destroy(&f->runtime);
        f->inject_initialized = false;
    }
    xr_task_runtime_destroy_all(&f->runtime);
    if (f->task_lock_initialized) {
        xr_mutex_destroy(&f->runtime.task_lock);
        f->task_lock_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

typedef struct StealFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker workers[2];
    XrMachine machines[2];
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool inject_initialized;
    bool worker_initialized[2];
    bool task_lock_initialized;
} StealFixture;

static bool steal_fixture_init(StealFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    fixture_init_runtime(&f->runtime, &f->isolate_storage, f->workers, f->machines, 2);
    xr_injectq_init(&f->runtime);
    f->inject_initialized = true;
    f->task_lock_initialized = true;

    for (int i = 0; i < 2; i++) {
        xr_worker_init(&f->workers[i], i, &f->runtime);
        f->worker_initialized[i] = true;
        f->workers[i].p.rng_state = (uint32_t) (0x12345678u + (uint32_t) i);
    }

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = NULL;
    tls_current_machine = NULL;
    return true;
}

static void steal_fixture_cleanup(StealFixture *f) {
    xr_runtime_stop(&f->runtime);
    for (int i = 1; i >= 0; i--) {
        if (f->worker_initialized[i]) {
            xr_worker_destroy(&f->workers[i]);
            f->worker_initialized[i] = false;
        }
    }
    if (f->inject_initialized) {
        xr_injectq_destroy(&f->runtime);
        f->inject_initialized = false;
    }
    xr_task_runtime_destroy_all(&f->runtime);
    if (f->task_lock_initialized) {
        xr_mutex_destroy(&f->runtime.task_lock);
        f->task_lock_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static void init_ready_coro(XrCoroutine *coro, int id, XrayIsolate *isolate) {
    memset(coro, 0, sizeof(*coro));
    coro->id = id;
    coro->isolate = isolate;
    atomic_store(&coro->flags, XR_CORO_FLG_READY);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
}

typedef enum SpawnProbeAfterSpawns {
    SPAWN_PROBE_DONE,
    SPAWN_PROBE_YIELD_ONCE,
    SPAWN_PROBE_BLOCK_ONCE,
} SpawnProbeAfterSpawns;

typedef struct SpawnProbeState {
    XrCoroutine *children;
    int child_count;
    int next_child;
    SpawnProbeAfterSpawns after_spawns;
    bool yielded;
} SpawnProbeState;

static XrCoroRunResult spawn_probe_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                          const XrCoroRunContext *run_ctx) {
    (void) event;
    (void) run_ctx;
    SpawnProbeState *state = (SpawnProbeState *) coro->backend_state;
    if (state && state->next_child < state->child_count) {
        return xr_coro_run_spawn_child(&state->children[state->next_child++]);
    }
    if (state && state->after_spawns == SPAWN_PROBE_YIELD_ONCE && !state->yielded) {
        state->yielded = true;
        return xr_coro_run_result(XR_CORO_RUN_YIELD);
    }
    if (state && state->after_spawns == SPAWN_PROBE_BLOCK_ONCE && !state->yielded) {
        state->yielded = true;
        xr_coro_transition_to_blocked(coro);
        return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
    }
    coro->result = XR_NULL_VAL;
    return xr_coro_run_done(XR_NULL_VAL);
}

static const XrCoroBackendVTable spawn_probe_backend = {
    .kind = XR_CORO_BACKEND_NATIVE,
    .resume = spawn_probe_resume,
};

static void init_spawn_probe_parent(XrCoroutine *parent, int id, XrayIsolate *isolate,
                                    SpawnProbeState *state) {
    init_ready_coro(parent, id, isolate);
    parent->backend = &spawn_probe_backend;
    parent->backend_state = state;
    parent->reductions = XR_CORO_REDUCTIONS;
}

static void init_spawn_probe_children(XrCoroutine *children, int count, int base_id,
                                      XrayIsolate *isolate) {
    for (int i = 0; i < count; i++) {
        init_ready_coro(&children[i], base_id + i, isolate);
        children[i].backend = &spawn_probe_backend;
        children[i].reductions = XR_CORO_REDUCTIONS;
    }
}

TEST(local_runq_pops_recent_owner_items_first) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine a;
    XrCoroutine b;
    XrCoroutine c;
    init_ready_coro(&a, 101, &f.isolate_storage);
    init_ready_coro(&b, 102, &f.isolate_storage);
    init_ready_coro(&c, 103, &f.isolate_storage);

    xr_worker_push(&f.worker, &a);
    xr_worker_push(&f.worker, &b);
    xr_worker_push(&f.worker, &c);

    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &c);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &b);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &a);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), NULL);
    ASSERT_EQ_INT(xr_proc_local_runq_len(&f.worker.p), 0);

    scheduler_fixture_cleanup(&f);
}

TEST(lifo_budget_flushes_run_next_to_local_queue) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine coro;
    init_ready_coro(&coro, 201, &f.isolate_storage);

    xr_worker_push_lifo(&f.worker, &coro);
    f.worker.p.lifo_polls = XR_MAX_LIFO_POLLS;

    ASSERT_EQ_PTR(xr_worker_try_pop_lifo(&f.worker, true), NULL);
    ASSERT_EQ_INT((int) f.worker.p.stats.lifo_flush_count, 1);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &coro);

    scheduler_fixture_cleanup(&f);
}

TEST(global_inject_spill_preserves_all_work) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    enum {
        TOTAL = XR_LOCAL_QUEUE_SIZE + 20
    };
    XrCoroutine coros[TOTAL];
    for (int i = 0; i < TOTAL; i++) {
        init_ready_coro(&coros[i], 300 + i, &f.isolate_storage);
        xr_worker_push(&f.worker, &coros[i]);
    }

    int inject_len = atomic_load_explicit(&f.runtime.injectq.len, memory_order_relaxed);
    ASSERT_TRUE(inject_len > 0);
    ASSERT_TRUE(atomic_load_explicit(&f.runtime.injectq_nonempty, memory_order_acquire));

    int pulled = worker_pull_inject(&f.worker, XR_INJECT_POP_BATCH);
    ASSERT_TRUE(pulled > 0);
    ASSERT_TRUE(xr_proc_local_runq_len(&f.worker.p) > 0);

    scheduler_fixture_cleanup(&f);
}

TEST(coro_ext_init_sets_timer_and_owner_sentinels) {
    XrCoroutine coro;
    init_ready_coro(&coro, 350, NULL);

    XrCoroExt *ext = xr_coro_ensure_ext(&coro);
    ASSERT_TRUE(ext != NULL);
    ASSERT_EQ_INT(ext->locked_worker, -1);
    ASSERT_EQ_INT(ext->wait_bucket_owner, -1);
    ASSERT_EQ_INT(ext->timer.slot, XR_TW_SLOT_INACTIVE);
    ASSERT_EQ_INT(ext->timer.owner_worker_id, -1);
    ASSERT_EQ_INT(ext->timer_wheel_owner, -1);
    ASSERT_FALSE(atomic_load_explicit(&ext->timer_active, memory_order_relaxed));
    ASSERT_EQ_INT((int) atomic_load_explicit(&ext->timer.state, memory_order_relaxed),
                  XR_TIMER_STATE_ACTIVE);

    xr_free(ext);
}

TEST(work_stealing_moves_batch_and_returns_direct_item) {
    StealFixture f;
    ASSERT_TRUE(steal_fixture_init(&f));

    enum {
        TOTAL = 16
    };
    XrCoroutine coros[TOTAL];
    for (int i = 0; i < TOTAL; i++) {
        init_ready_coro(&coros[i], 400 + i, &f.isolate_storage);
        xr_worker_push(&f.workers[0], &coros[i]);
    }
    int64_t old_submit_time = xr_monotonic_ticks() - XR_STEAL_TIME_RESOLUTION_MS - 1;
    for (int i = 0; i < TOTAL; i++) {
        coros[i].submit_time = old_submit_time;
    }

    int64_t delay = 0;
    bool should_exit = false;
    XrCoroutine *stolen = xr_worker_try_steal_once(&f.workers[1], &f.runtime, &f.runtime.running,
                                                   &delay, &should_exit);

    ASSERT_TRUE(!should_exit);
    ASSERT_TRUE(stolen != NULL);
    ASSERT_TRUE((int) f.workers[1].p.stats.steal_success_count > 0);
    ASSERT_TRUE((int) f.workers[1].p.stats.stolen_count > 0);
    ASSERT_TRUE(xr_proc_local_runq_len(&f.workers[0].p) < TOTAL);

    steal_fixture_cleanup(&f);
}

TEST(spawn_burst_shares_same_parent_fanout) {
    StealFixture f;
    ASSERT_TRUE(steal_fixture_init(&f));
    tls_current_worker = &f.workers[0];
    tls_current_machine = f.workers[0].m;

    XrCoroutine parent;
    XrCoroutine children[3];
    SpawnProbeState state = {
        .children = children, .child_count = 3, .next_child = 0, .after_spawns = SPAWN_PROBE_DONE};
    init_spawn_probe_children(children, 3, 600, &f.isolate_storage);
    init_spawn_probe_parent(&parent, 500, &f.isolate_storage, &state);

    worker_exec_with_cont_stealing(&f.workers[0], &parent);

    ASSERT_EQ_INT(state.next_child, 3);
    ASSERT_TRUE(xr_coro_flags_has(&parent, XR_CORO_FLG_DONE));
    ASSERT_EQ_INT(parent.spawn_burst_count, 0);
    ASSERT_TRUE(xr_coro_flags_has(&children[0], XR_CORO_FLG_DONE));
    ASSERT_FALSE(xr_coro_flags_has(&children[1], XR_CORO_FLG_DONE));
    ASSERT_FALSE(xr_coro_flags_has(&children[2], XR_CORO_FLG_DONE));
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_inline_child_count, 1);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_shared_child_count, 2);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_burst_shared_count, 2);
    ASSERT_EQ_INT(xr_proc_local_runq_len(&f.workers[0].p), 2);

    steal_fixture_cleanup(&f);
}

TEST(spawn_burst_resets_after_yield) {
    StealFixture f;
    ASSERT_TRUE(steal_fixture_init(&f));
    tls_current_worker = &f.workers[0];
    tls_current_machine = f.workers[0].m;

    XrCoroutine parent;
    XrCoroutine first_child[1];
    XrCoroutine second_child[1];
    SpawnProbeState first_state = {.children = first_child,
                                   .child_count = 1,
                                   .next_child = 0,
                                   .after_spawns = SPAWN_PROBE_YIELD_ONCE};
    SpawnProbeState second_state = {.children = second_child,
                                    .child_count = 1,
                                    .next_child = 0,
                                    .after_spawns = SPAWN_PROBE_DONE};
    init_spawn_probe_children(first_child, 1, 700, &f.isolate_storage);
    init_spawn_probe_children(second_child, 1, 710, &f.isolate_storage);
    init_spawn_probe_parent(&parent, 650, &f.isolate_storage, &first_state);

    worker_exec_with_cont_stealing(&f.workers[0], &parent);

    ASSERT_EQ_INT(first_state.next_child, 1);
    ASSERT_TRUE(xr_coro_flags_has(&first_child[0], XR_CORO_FLG_DONE));
    ASSERT_FALSE(xr_coro_flags_has(&parent, XR_CORO_FLG_DONE));
    ASSERT_EQ_INT(parent.spawn_burst_count, 0);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_inline_child_count, 1);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_shared_child_count, 0);

    // Yield-storm diffusion may route the yielded parent through the global
    // inject queue (streak >= worker_count/2); pull it like a real worker.
    XrCoroutine *resumed = xr_worker_pop(&f.workers[0]);
    if (!resumed) {
        worker_pull_inject(&f.workers[0], XR_INJECT_POP_BATCH);
        resumed = xr_worker_pop(&f.workers[0]);
    }
    ASSERT_EQ_PTR(resumed, &parent);
    parent.backend_state = &second_state;
    worker_exec_with_cont_stealing(&f.workers[0], &parent);

    ASSERT_EQ_INT(second_state.next_child, 1);
    ASSERT_TRUE(xr_coro_flags_has(&second_child[0], XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&parent, XR_CORO_FLG_DONE));
    ASSERT_EQ_INT(parent.spawn_burst_count, 0);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_inline_child_count, 2);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_shared_child_count, 0);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_burst_shared_count, 0);

    steal_fixture_cleanup(&f);
}

TEST(spawn_burst_resets_after_block) {
    StealFixture f;
    ASSERT_TRUE(steal_fixture_init(&f));
    tls_current_worker = &f.workers[0];
    tls_current_machine = f.workers[0].m;

    XrCoroutine parent;
    XrCoroutine first_child[1];
    XrCoroutine second_child[1];
    SpawnProbeState first_state = {.children = first_child,
                                   .child_count = 1,
                                   .next_child = 0,
                                   .after_spawns = SPAWN_PROBE_BLOCK_ONCE};
    SpawnProbeState second_state = {.children = second_child,
                                    .child_count = 1,
                                    .next_child = 0,
                                    .after_spawns = SPAWN_PROBE_DONE};
    init_spawn_probe_children(first_child, 1, 800, &f.isolate_storage);
    init_spawn_probe_children(second_child, 1, 810, &f.isolate_storage);
    init_spawn_probe_parent(&parent, 750, &f.isolate_storage, &first_state);

    worker_exec_with_cont_stealing(&f.workers[0], &parent);

    ASSERT_EQ_INT(first_state.next_child, 1);
    ASSERT_TRUE(xr_coro_flags_has(&first_child[0], XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&parent, XR_CORO_FLG_BLOCKED));
    /* A blocked coro may already be owned by a concurrent waker, so the
     * executor result path no longer touches it: the burst counter keeps
     * its value at block time and resets on the next enqueue (the point
     * where the enqueuing thread owns the coro). */
    ASSERT_EQ_INT(parent.spawn_burst_count, 1);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_inline_child_count, 1);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_shared_child_count, 0);

    ASSERT_TRUE(xr_coro_claim_wake(&parent));
    parent.backend_state = &second_state;
    xr_worker_push(&f.workers[0], &parent);
    ASSERT_EQ_INT(parent.spawn_burst_count, 0);
    XrCoroutine *resumed = xr_worker_pop(&f.workers[0]);
    ASSERT_EQ_PTR(resumed, &parent);
    worker_exec_with_cont_stealing(&f.workers[0], &parent);

    ASSERT_EQ_INT(second_state.next_child, 1);
    ASSERT_TRUE(xr_coro_flags_has(&second_child[0], XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&parent, XR_CORO_FLG_DONE));
    ASSERT_EQ_INT(parent.spawn_burst_count, 0);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_inline_child_count, 2);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_shared_child_count, 0);
    ASSERT_EQ_INT((int) f.workers[0].p.stats.spawn_burst_shared_count, 0);

    steal_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Scheduler Run Queue");
RUN_TEST(local_runq_pops_recent_owner_items_first);
RUN_TEST(lifo_budget_flushes_run_next_to_local_queue);
RUN_TEST(global_inject_spill_preserves_all_work);
RUN_TEST(coro_ext_init_sets_timer_and_owner_sentinels);
RUN_TEST(work_stealing_moves_batch_and_returns_direct_item);
RUN_TEST(spawn_burst_shares_same_parent_fanout);
RUN_TEST(spawn_burst_resets_after_yield);
RUN_TEST(spawn_burst_resets_after_block);

TEST_MAIN_END()
