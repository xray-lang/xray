/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_compiler.c - Complete Source and preparation allocation failures
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "deep compiler %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
XR_DATA const XrXirProgramSpec worker_deep_fixture_program;
#include "xir_mixed_deep_compile.h"
static unsigned deep_compiler_number(const char *text) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && value < 20000); return (unsigned)value;
}
static void deep_compiler_bounds(unsigned keeper, const DeepCompileResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = deep_compile_caps();
        if (!axis) caps.allocated_bytes = exact[axis] - minus;
        else if (axis == 1) caps.live_bytes = exact[axis] - minus;
        else caps.work = exact[axis] - minus;
        DeepCompileResult run = deep_compile_operation(keeper, SIZE_MAX, caps, NULL);
        if (run.status != (minus ? XR_XIR_BUDGET : XR_XIR_OK)) fprintf(stderr, "Deep compiler axis=%u minus=%u stage=%u status=%u\n", axis, minus, run.stage, run.status);
        CHECK(run.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(run.stats.allocated_bytes <= caps.allocated_bytes && run.stats.peak_bytes <= caps.live_bytes && run.stats.work <= caps.work);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 6); unsigned keeper = deep_compiler_number(argv[1]); CHECK(keeper < 2);
    const char *dump = argc == 4 ? argv[3] : NULL;
    DeepCompileResult normal = deep_compile_operation(keeper, SIZE_MAX, deep_compile_caps(), dump);
    printf("Deep compiler keeper=%u normal status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu, oneFiniteOwner sourceDeath packetPoison bothPrepared lastImageMetadataAfterCallerDeath physical0/0\n",
        keeper, normal.status, normal.stage, normal.sites, (unsigned long long)normal.stats.allocated_bytes,
        (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work, normal.c_bytes); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.stage == 10 && normal.sites && normal.sites < 20000);
    if (!strcmp(argv[2], "baseline")) { CHECK(argc == 3 || argc == 4); return 0; }
    if (!strcmp(argv[2], "axes")) { CHECK(argc == 3); deep_compiler_bounds(keeper, &normal); return 0; }
    CHECK(argc == 6 && !strcmp(argv[2], "fi"));
    size_t begin = deep_compiler_number(argv[3]), end = deep_compiler_number(argv[4]), expected = deep_compiler_number(argv[5]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        DeepCompileResult run = deep_compile_operation(keeper, site, deep_compile_caps(), NULL);
        if (run.status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "Deep compiler FI keeper=%u site=%zu stage=%u status=%u hit=%u attempts=%zu\n",
            keeper, site, run.stage, run.status, (unsigned)run.injected, run.sites);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && run.injected && run.sites > site);
    }
    printf("Deep compiler complete FI keeper=%u range=%zu:%zu of%zu allActualOrdinal typedOOM physical0/0\n", keeper, begin, end, expected);
    return 0;
}
