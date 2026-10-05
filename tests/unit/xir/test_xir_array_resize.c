/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_resize.c - Complete bounded resize programs
 *
 * KEY CONCEPT:
 *   VM and native code independently consume identical complete sealed programs.
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
#if defined(XR_RESIZE_NATIVE)
XR_DATA const XrXirProgramSpec array_resize_program;
#endif
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_array_resize_pipeline.h"
static uint32_t resize_find(const XrXirModule *module, const char *name) {
    const size_t length = strlen(name);
    uint32_t result = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *decl = &module->functions[f];
        if (decl->name_length == length && !memcmp(decl->name, name, length)) {
            CHECK(result == UINT32_MAX); result = f;
        }
    }
    CHECK(result != UINT32_MAX); return result;
}
static XrXirValue resize_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
#if defined(XR_RESIZE_MATRIX) && XR_RESIZE_MATRIX == 7
#include "xir_array_resize_error_matrix.h"
#elif defined(XR_RESIZE_MATRIX) && XR_RESIZE_MATRIX == 6
#include "xir_array_resize_shadow.h"
#elif defined(XR_RESIZE_MATRIX)
#include "xir_array_resize_matrix.h"
#else
#include "xir_array_resize_cases.h"
#include "xir_array_resize_escape.h"
#include "xir_array_resize_limits.h"
#endif
#include "xir_array_resize_compiler.h"
#include "xir_array_resize_rejections.h"
#include "xir_array_resize_initialization.h"
int main(int argc, char **argv) {
    if (resize_parallel_cli(argc, argv)) return 0;
    if (argc == 2 && !strcmp(argv[1], "--rejections")) { resize_rejections(); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--profile-compiler")) {
        resize_profile = true; resize_profile_clock = clock();
        ResizeCompile profile = resize_build(SIZE_MAX, resize_limits(), (unsigned)strtoul(argv[2], NULL, 10), NULL);
        CHECK(profile.status == XR_XIR_OK && profile.program); resize_release(&profile); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--compiler")) { resize_compiler((unsigned)strtoul(argv[2], NULL, 10)); return 0; }
    unsigned mode = argc > 1 ? (unsigned) strtoul(argv[1], NULL, 10) : 0;
    const char *emit = argc == 3 ? argv[2] : NULL;
    ResizeCompile run = resize_build(SIZE_MAX, resize_limits(), mode, emit);
    if (run.status != XR_XIR_OK) fprintf(stderr, "build status=%u stage=%u attempts=%zu allocated=%llu peak=%llu work=%llu\n",
        run.status, run.stage, run.sites, (unsigned long long)run.stats.allocated_bytes,
        (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    CHECK(run.status == XR_XIR_OK);
    if (!emit) { CHECK(run.program); resize_initialization_faults(&run); resize_goldens(&run);
#if !defined(XR_RESIZE_MATRIX)
        resize_runtime_faults(&run, false); resize_runtime_faults(&run, true);
        resize_extra_faults(&run);
        resize_runtime_limits(&run); resize_float_bits(&run); resize_escaped(&run);
#endif
    }
    printf("resize baseline mode%u sites=%zu allocated=%llu peak=%llu work=%llu\n", mode, run.sites,
        (unsigned long long)run.stats.allocated_bytes, (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    resize_release(&run); instance_compile_report(); return 0;
}
