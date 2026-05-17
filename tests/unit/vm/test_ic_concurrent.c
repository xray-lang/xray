/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ic_concurrent.c - Stress the per-coroutine IC subsystem from
 *     multiple OS threads at once. The whole point of moving ICs off
 *     XrProto onto each XrVMContext is that two workers running the
 *     same proto must never touch the same IC bytes; this test gives
 *     ASan / TSan a chance to catch any residual sharing.
 *
 *   Layout:
 *     - One shared XrProto (heavy IC table sizes) created on the main
 *       thread; immutable after creation, only its proto_id is read.
 *     - N worker threads, each owning a stack-local XrVMContext.
 *       Every iteration:
 *         * ensure_ic_fields / ensure_ic_methods on the shared proto
 *         * mutate slot[0] of each table with a thread-local sentinel
 *         * read slot[0] back and re-confirm the sentinel
 *         * snapshot, free snapshot
 *         * occasionally tear the ctx down and rebuild it
 *     - After all workers join, every error counter must be zero.
 *
 *   The shared proto is kept const-ish: workers never poke its
 *   bytes. If they did, this is exactly where TSan would scream.
 */

#include "../test_framework.h"

#include "runtime/value/xchunk.h"
#include "runtime/xexec_frame.h"
#include "vm/xvm_internal.h"
#include "vm/xic_field.h"
#include "vm/xic_field_table.h"
#include "vm/xic_method.h"
#include "base/xmalloc.h"

#include "os/os_thread.h"
#include <stdlib.h> /* qsort */
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* Tunables — kept moderate so the test stays well below 1s on CI. */
#define IC_STRESS_THREADS 4
#define IC_STRESS_ITERATIONS 5000
#define IC_STRESS_PROTO_INSTS 16

typedef struct {
    XrProto *shared_proto;   /* owned by main thread */
    int tid;                 /* 0..N-1, used as sentinel */
    _Atomic uint64_t errors; /* incremented on any mismatch */
} IcStressArg;

/* Per-thread workload: own ctx, hammer the shared proto's IC slots
 * but only inside that ctx's tables. */
static void *ic_stress_worker(void *raw) {
    IcStressArg *arg = (IcStressArg *) raw;
    XrProto *p = arg->shared_proto;
    /* A recognisable per-thread sentinel that should never appear in
     * any other thread's tables; if it does, ICs are leaking across
     * ctx boundaries. */
    int sentinel_field = 0xF000 + arg->tid;
    int sentinel_method = 0xC000 + arg->tid;

    for (int i = 0; i < IC_STRESS_ITERATIONS; i++) {
        XrVMContext ctx;
        memset(&ctx, 0, sizeof(ctx));

        XrICFieldTable *ft = xr_vm_ctx_ensure_ic_fields(&ctx, p);
        XrICMethodTable *mt = xr_vm_ctx_ensure_ic_methods(&ctx, p);
        if (!ft || !mt) {
            atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
            continue;
        }
        if (ft->count != IC_STRESS_PROTO_INSTS || mt->count != IC_STRESS_PROTO_INSTS) {
            atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
        }

        /* Mutate a local IC slot with the per-thread sentinel. */
        ft->caches[0].cached_symbol = sentinel_field;
        ft->caches[0].state = XR_IC_FIELD_MONO;
        mt->caches[0].total_count = (uint32_t) sentinel_method;

        /* Take a snapshot — these run on a foreground thread in
         * production, so we exercise the hot path here. */
        XrICFieldTable *fs = xr_vm_ic_fields_snapshot(&ctx, p);
        XrICMethodTable *ms = xr_vm_ic_methods_snapshot(&ctx, p);
        if (!fs || !ms) {
            atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
        } else {
            /* Snapshot must reflect THIS thread's writes, not some
             * other thread's. */
            if (fs->caches[0].cached_symbol != sentinel_field ||
                ms->caches[0].total_count != (uint32_t) sentinel_method) {
                atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
            }
            xr_ic_field_table_free(fs);
            xr_ic_method_table_free(ms);
        }

        /* Read live slot back; must still equal our sentinel — if
         * another thread had mutated this ctx, the sentinel would
         * change. */
        if (ft->caches[0].cached_symbol != sentinel_field ||
            mt->caches[0].total_count != (uint32_t) sentinel_method) {
            atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
        }

        /* Tear the ctx down half the iterations to exercise the
         * alloc/free path under contention. */
        if (i & 1) {
            xr_vm_ctx_free_ic_tables(&ctx);
            if (ctx.ic_tables_capacity != 0 || ctx.ic_field_tables != NULL ||
                ctx.ic_method_tables != NULL) {
                atomic_fetch_add_explicit(&arg->errors, 1u, memory_order_relaxed);
            }
        } else {
            xr_vm_ctx_free_ic_tables(&ctx);
        }
    }
    return NULL;
}

TEST(ic_concurrent_workers_stay_isolated) {
    XrProto *shared = xr_vm_proto_new();
    ASSERT_NOT_NULL(shared);
    for (int i = 0; i < IC_STRESS_PROTO_INSTS; i++) {
        xr_vm_proto_write(shared, (XrInstruction) 0, 1);
    }

    xr_thread_t threads[IC_STRESS_THREADS];
    IcStressArg args[IC_STRESS_THREADS];
    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        args[t].shared_proto = shared;
        args[t].tid = t;
        atomic_store_explicit(&args[t].errors, 0u, memory_order_relaxed);
        bool ok = xr_thread_create(&threads[t], ic_stress_worker, &args[t]);
        ASSERT_TRUE(ok);
    }
    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        xr_thread_join(threads[t], NULL);
    }

    uint64_t total_errors = 0;
    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        total_errors += atomic_load_explicit(&args[t].errors, memory_order_relaxed);
    }
    ASSERT_EQ_INT((long long) total_errors, 0LL);

    xr_vm_proto_free(shared);
}

/* A second case targets the proto_id allocation hot path: every
 * thread spins through xr_vm_proto_new / xr_vm_proto_free creating
 * fresh protos. The atomic counter underneath proto_id must hand
 * each thread a unique id, so collisions show up as duplicates in
 * the merged set.
 *
 * We use a simple bitset to detect duplicates without dragging in
 * a hash structure: each thread records the ids it created, then
 * the main thread merges and sorts to spot collisions. */

#define IC_PROTO_ID_PER_THREAD 200

typedef struct {
    uint32_t ids[IC_PROTO_ID_PER_THREAD];
} IdRecord;

static void *proto_id_alloc_worker(void *raw) {
    IdRecord *rec = (IdRecord *) raw;
    for (int i = 0; i < IC_PROTO_ID_PER_THREAD; i++) {
        XrProto *p = xr_vm_proto_new();
        rec->ids[i] = p ? p->proto_id : 0u;
        xr_vm_proto_free(p);
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *) a;
    uint32_t y = *(const uint32_t *) b;
    return (x < y) ? -1 : (x > y);
}

TEST(proto_id_allocation_is_race_free) {
    xr_thread_t threads[IC_STRESS_THREADS];
    IdRecord records[IC_STRESS_THREADS];

    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        memset(&records[t], 0, sizeof(records[t]));
        bool ok = xr_thread_create(&threads[t], proto_id_alloc_worker, &records[t]);
        ASSERT_TRUE(ok);
    }
    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        xr_thread_join(threads[t], NULL);
    }

    /* Merge all ids into one buffer and sort to detect duplicates. */
    int total = IC_STRESS_THREADS * IC_PROTO_ID_PER_THREAD;
    uint32_t *all = (uint32_t *) xr_malloc((size_t) total * sizeof(uint32_t));
    ASSERT_NOT_NULL(all);
    int n = 0;
    for (int t = 0; t < IC_STRESS_THREADS; t++) {
        for (int i = 0; i < IC_PROTO_ID_PER_THREAD; i++) {
            all[n++] = records[t].ids[i];
        }
    }
    qsort(all, (size_t) total, sizeof(uint32_t), cmp_u32);

    int duplicates = 0;
    for (int i = 1; i < total; i++) {
        if (all[i] == all[i - 1])
            duplicates++;
    }
    xr_free(all);

    ASSERT_EQ_INT(duplicates, 0);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Per-ctx IC under thread contention");
RUN_TEST(ic_concurrent_workers_stay_isolated);

RUN_TEST_SUITE("proto_id atomic allocation");
RUN_TEST(proto_id_allocation_is_race_free);
TEST_MAIN_END()
