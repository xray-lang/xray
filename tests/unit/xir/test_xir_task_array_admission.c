/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_array_admission.c - Independent element and escaped-owner oracles
 *
 * KEY CONCEPT:
 *   Complete source programs own every result beyond their original execution owners.
 */

#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_nullable.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#if defined(XR_TASK_ARRAY_NATIVE)
XR_DATA const XrXirProgramSpec task_array_admission_program;
#endif
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_task_budget.c"
#include "xir_task_array_admission_pipeline.h"
static uint32_t matrix_find(const XrXirModule *module, const char *name) {
    const size_t length = strlen(name); uint32_t result = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *decl = &module->functions[f];
        if (decl->name_length == length && !memcmp(decl->name, name, length)) {
            CHECK(result == UINT32_MAX); result = f;
        }
    }
    CHECK(result != UINT32_MAX); return result;
}
static XrXirValue matrix_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED); return result;
}
#include "xir_task_array_admission_cases.h"
int main(int argc, char **argv) {
    CHECK(argc >= 1 && argc <= 3);
    const unsigned mode = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0;
    CHECK(mode <= 3);
    const char *emit = argc == 3 ? argv[2] : NULL;
    TaskArrayCompile run = task_array_build(SIZE_MAX, task_array_limits(), mode, emit);
    if (run.status != XR_XIR_OK) fprintf(stderr, "Task array%d build status%u stage%u sites%zu\n",
        XR_TASK_ARRAY_GROUP, run.status, run.stage, run.sites);
    CHECK(run.status == XR_XIR_OK);
    if (!emit) { CHECK(run.program); task_array_guards(&run); matrix_goldens(&run); }
    printf("Task array%d baseline mode%u sites%zu allocated%llu peak%llu work%llu Cbytes%zu\n",
        XR_TASK_ARRAY_GROUP, mode, run.sites, (unsigned long long)run.stats.allocated_bytes,
        (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work, run.c_bytes);
    task_array_release(&run); instance_compile_report(); return 0;
}
