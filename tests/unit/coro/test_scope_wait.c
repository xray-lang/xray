/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scope_wait.c - Unit tests for scope wait lifecycle tokens
 */

#include "../test_framework.h"
#include "coro/xblock.h"
#include "coro/xcoroutine.h"
#include "coro/xtask.h"
#include "coro/xworker_internal.h"
#include "runtime/mem/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct ScopeFixture {
    XrayIsolate isolate_storage;
    XrRuntimeCore core;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker worker;
    XrMachine machine;
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool worker_initialized;
} ScopeFixture;

static bool scope_fixture_init(ScopeFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL)) {
        return false;
    }
    f->sys_heap_initialized = true;
    f->core.sys_heap = &f->sys_heap;
    f->isolate_storage.core_rt = &f->core;
    f->core.vm_owner = &f->isolate_storage;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    f->runtime.core = &f->core;
    xr_scheduler_runtime_attach_isolate(&f->runtime, &f->isolate_storage);
    f->runtime.worker_count = 1;
    f->runtime.workers = &f->worker;
    f->runtime.machines = &f->machine;
    atomic_store(&f->runtime.running, true);
    atomic_store(&f->runtime.threads_started, false);

    xr_worker_init(&f->worker, 0, &f->runtime);
    f->worker_initialized = true;

    f->isolate_storage.vm.scheduler = &f->runtime;
    tls_current_worker = &f->worker;
    tls_current_machine = &f->machine;
    return true;
}

static void scope_fixture_cleanup(ScopeFixture *f) {
    if (f->worker_initialized) {
        xr_worker_destroy(&f->worker);
        f->worker_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static void init_running_scope_coro(XrCoroutine *coro, XrCoroExt *ext, int id,
                                    XrayIsolate *isolate) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = id;
    coro->core = isolate ? isolate->core_rt : NULL;
    coro->scheduler =
        (isolate && isolate->vm.scheduler) ? (XrRuntime *) isolate->vm.scheduler : NULL;
    coro->ext = ext;
    atomic_store(&coro->flags, XR_CORO_FLG_RUNNING);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
}

TEST(scope_wait_token_tracks_block_wake_and_exit) {
    ScopeFixture f;
    ASSERT_TRUE(scope_fixture_init(&f));

    XrCoroutine owner;
    XrCoroutine child;
    XrCoroExt owner_ext;
    XrCoroExt child_ext;
    init_running_scope_coro(&owner, &owner_ext, 501, &f.isolate_storage);
    init_running_scope_coro(&child, &child_ext, 502, &f.isolate_storage);

    XrCoroBlockResult entered = xr_coro_scope_enter(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) entered.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_NOT_NULL(owner.current_scope);

    XrScopeContext *scope = owner.current_scope;
    ASSERT_TRUE(xr_coro_set_parent_scope(&child, scope));
    atomic_fetch_add(&scope->count, 1);

    XrCoroBlockResult blocked = xr_coro_scope_exit(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_REGISTERED);
    ASSERT_EQ_PTR(atomic_load(&owner_ext.wait.scope_token.scope), scope);
    ASSERT_TRUE(xr_coro_flags_has(&owner, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&owner.flags)), XR_CORO_STATE_BLOCKED);
    ASSERT_EQ_INT(xr_coro_get_wait_reason(xr_coro_flags_load(&owner)),
                  XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT);
    xr_coro_wake_waiter(&f.isolate_storage, &child);

    ASSERT_EQ_INT(atomic_load(&scope->count), 0);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_RESOLVED);
    ASSERT_TRUE(xr_coro_flags_has(&owner, XR_CORO_FLG_READY));
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &owner);
    ASSERT_NULL(xr_coro_parent_scope(&child));

    XrCoroBlockResult resumed = xr_coro_scope_exit(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_NULL(owner.current_scope);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_IDLE);
    ASSERT_NULL(atomic_load(&owner_ext.wait.scope_token.scope));

    scope_fixture_cleanup(&f);
}

TEST(scope_wait_token_resolves_after_published_block) {
    ScopeFixture f;
    ASSERT_TRUE(scope_fixture_init(&f));

    XrCoroutine owner;
    XrCoroutine child;
    XrCoroExt owner_ext;
    XrCoroExt child_ext;
    init_running_scope_coro(&owner, &owner_ext, 601, &f.isolate_storage);
    init_running_scope_coro(&child, &child_ext, 602, &f.isolate_storage);

    XrCoroBlockResult entered = xr_coro_scope_enter(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) entered.kind, (int) XR_CORO_BLOCK_READY);

    XrScopeContext *scope = owner.current_scope;
    ASSERT_NOT_NULL(scope);
    ASSERT_TRUE(xr_coro_set_parent_scope(&child, scope));
    atomic_fetch_add(&scope->count, 1);

    XrCoroBlockResult blocked = xr_coro_scope_exit(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_REGISTERED);
    ASSERT_TRUE(xr_coro_flags_has(&owner, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&owner.flags)), XR_CORO_STATE_BLOCKED);

    xr_coro_wake_waiter(&f.isolate_storage, &child);
    ASSERT_EQ_INT(atomic_load(&scope->count), 0);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_RESOLVED);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &owner);

    XrCoroBlockResult resumed = xr_coro_scope_exit(&owner, XR_SCOPE_WAIT);
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_NULL(owner.current_scope);
    ASSERT_EQ_INT(atomic_load(&owner_ext.wait.scope_token.state), XR_SCOPE_WAIT_IDLE);

    scope_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Scope Wait");
RUN_TEST(scope_wait_token_tracks_block_wake_and_exit);
RUN_TEST(scope_wait_token_resolves_after_published_block);

TEST_MAIN_END()
