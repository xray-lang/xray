/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_wait_resources.c - Child waits preserve finite ledgers through retirement
 */
#define XR_WORKER_WAIT_RESOURCE_MAIN
#include "test_xir_mixed_worker_waits.c"
#include "xir_mixed_retained_faults.h"
static void wait_resources_external(XrXirInstance *instance, const XrXirInstanceResult *event, bool resume) {
    CHECK(event->outcome.status == XR_XIR_CALL_SUSPENDED && event->epoch && event->outcome.wake);
    XrXirWaitRequest request = {0};
    CHECK(xr_xir_instance_wait_request(instance, event->epoch, event->outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == (uint32_t)XR_XIR_WAIT_TIMER_MS && request.after_ms == 7);
    CHECK(!request.reserved && !request.subject && !request.generation && !request.ticket);
    if (resume) {
        CHECK(xr_xir_instance_resume(instance, event->epoch, event->outcome.wake) == XR_XIR_CALL_READY);
        worker_wait_reject(instance, event->epoch, event->outcome.wake);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 4); (void)worker_wait_main;
    const WorkerControlCase *control = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 2; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) control = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(control && backend < 4); bool fi = !strcmp(argv[3], "fi"); CHECK(fi || !strcmp(argv[3], "axes"));
    RetainedFaultOracle oracle = {.control=control, .observed_instance=&worker_wait_instance,
        .observed_task=&worker_wait_task, .expect_pending=worker_wait_expect_pending, .check_value=worker_wait_bytes,
        .external_wait=!strcmp(control->name, "timer") ? wait_resources_external : NULL};
    XrXirProgram *program = worker_control_program(control, backend); retained_fault_axes(program, &oracle, backend, fi);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
