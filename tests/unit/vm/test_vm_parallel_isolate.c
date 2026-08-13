/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_parallel_isolate.c - Same-process multi-isolate VM parallel gate.
 *
 * The VM hosted parallel backend must be runtime/isolate scoped.  This test
 * runs two full VM isolates concurrently in one process; each isolate owns its
 * scheduler runtime and executes a real stdlib `parallel.Plan` batch.  A
 * process-global batch slot, shared lane storage, or cross-isolate lifecycle
 * leak tends to show up here as a failed assertion, deadlock, or crash.
 */

#include "../test_framework.h"

#include "api/xisolate_profile.h"
#include "os/os_thread.h"
#include "runtime/xisolate_api.h"
#include "xray_vm.h"

#include <stdatomic.h>

#define VM_PAR_ISOLATE_RUNNERS 2
#define VM_PAR_ISOLATE_WORKERS 4

static const char *VM_PAR_ISOLATE_SCRIPT =
    "import parallel\n"
    "\n"
    "enum VmParallelIsolateErr { CheckFailed }\n"
    "\n"
    "fn require(ok: bool) {\n"
    "    if (!ok) {\n"
    "        throw VmParallelIsolateErr.CheckFailed\n"
    "    }\n"
    "}\n"
    "\n"
    "const touched = Atomic<int>(0)\n"
    "const lane0 = Atomic<int>(0)\n"
    "const lane1 = Atomic<int>(0)\n"
    "const lane2 = Atomic<int>(0)\n"
    "const lane3 = Atomic<int>(0)\n"
    "\n"
    "var plan = parallel.Plan<int>(parallel.Options(4), (lane) -> lane)\n"
    "\n"
    "plan.forEach(0..24000, (state, i) -> {\n"
    "    touched.add(1, Ordering.Relaxed)\n"
    "    if (state == 0) {\n"
    "        lane0.store(1, Ordering.Relaxed)\n"
    "    } else if (state == 1) {\n"
    "        lane1.store(1, Ordering.Relaxed)\n"
    "    } else if (state == 2) {\n"
    "        lane2.store(1, Ordering.Relaxed)\n"
    "    } else if (state == 3) {\n"
    "        lane3.store(1, Ordering.Relaxed)\n"
    "    }\n"
    "})\n"
    "\n"
    "var lanesSeen = lane0.load(Ordering.Relaxed) + lane1.load(Ordering.Relaxed) +\n"
    "    lane2.load(Ordering.Relaxed) + lane3.load(Ordering.Relaxed)\n"
    "\n"
    "require(touched.load(Ordering.Relaxed) == 24000)\n"
    "require(lanesSeen == 4)\n";

typedef struct VmParallelIsolateRun {
    _Atomic int *ready_count;
    _Atomic bool *start;
    int result;
} VmParallelIsolateRun;

static void *vm_parallel_isolate_runner(void *raw) {
    VmParallelIsolateRun *run = (VmParallelIsolateRun *) raw;
    run->result = -1000;

    XrVMConfig params;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &params);
    XrVMRuntime *isolate = xr_isolate_profile_create(&params);
    if (!isolate) {
        run->result = -1001;
        return NULL;
    }

    xr_isolate_multicore_init(isolate, VM_PAR_ISOLATE_WORKERS);
    atomic_fetch_add_explicit(run->ready_count, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(run->start, memory_order_acquire))
        xr_thread_yield();

    run->result = xr_isolate_dostring(isolate, VM_PAR_ISOLATE_SCRIPT);

    xray_vm_multicore_destroy(isolate);
    xray_vm_delete(isolate);
    return NULL;
}

TEST(vm_parallel_batches_are_isolate_scoped_under_host_concurrency) {
    xr_thread_t threads[VM_PAR_ISOLATE_RUNNERS];
    VmParallelIsolateRun runs[VM_PAR_ISOLATE_RUNNERS];
    _Atomic int ready_count;
    _Atomic bool start;
    atomic_init(&ready_count, 0);
    atomic_init(&start, false);

    for (int i = 0; i < VM_PAR_ISOLATE_RUNNERS; i++) {
        runs[i].ready_count = &ready_count;
        runs[i].start = &start;
        runs[i].result = -1;
        ASSERT_TRUE(xr_thread_create(&threads[i], vm_parallel_isolate_runner, &runs[i]));
    }

    while (atomic_load_explicit(&ready_count, memory_order_acquire) < VM_PAR_ISOLATE_RUNNERS)
        xr_thread_yield();
    atomic_store_explicit(&start, true, memory_order_release);

    for (int i = 0; i < VM_PAR_ISOLATE_RUNNERS; i++)
        ASSERT_EQ_INT(xr_thread_join(threads[i], NULL), 0);
    for (int i = 0; i < VM_PAR_ISOLATE_RUNNERS; i++)
        ASSERT_EQ_INT(runs[i].result, 0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("VM parallel multi-isolate");
RUN_TEST(vm_parallel_batches_are_isolate_scoped_under_host_concurrency);
TEST_MAIN_END()
