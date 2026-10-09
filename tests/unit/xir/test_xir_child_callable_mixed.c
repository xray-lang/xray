/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_child_callable_mixed.c - Real child and closure edges cross providers
 *
 * KEY CONCEPT:
 *   One checked identity binds authentic native and VM bodies in one Program.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "child mixed consumer FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

XR_DATA const XrXirProgramSpec child_callable_native_program;
XR_DATA const XrXirCallEntry child_callable_native_entries[];
#include "xir_child_callable_native_oracles.h"
#include "xir_child_callable_mixed_oracles.h"

int main(int argc, char **argv) {
    _Static_assert(XR_XIR_CHECKED_CONTRACT == 70u, "current callable semantic contract");
    CHECK(argc == 2);
    bool root_native = !strcmp(argv[1], "native-vm");
    CHECK(root_native || !strcmp(argv[1], "vm-native"));
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    /* Shared native-only entry wrappers remain intact and are never executed here. */
    (void)&cn_build;
    (void)&cn_run_pair;
    CmFixture fixture = {0};
    cm_build(&fixture, root_native);
    cn_new_pair(&fixture.base);
    cm_run_pair(&fixture);
    cn_close(&fixture.base);
    CHECK(!fixture.lowered && fixture.base.code_releases == 1);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    cm_observed = NULL;
    printf("MIXED_CHILD_LOCAL direction=%s rootSteps=6 childSteps=10 relaySteps=4 pureSteps=4 rootAwait=2 childRelay=2 relayPure=2 ordinaryNone=2 capturedNone=2 rootUnknownMixedDenied=8 crossInstanceDenied=2 forgedDenied=6 occupiedUnchanged=2 typed42=2 bytes42=2 Instances=2 compilerRuntimePhysical=0/0 FIresources=OPEN\n", argv[1]);
    return 0;
}
