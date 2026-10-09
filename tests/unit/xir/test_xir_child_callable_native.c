/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_child_callable_native.c - Real native children call local closures
 *
 * KEY CONCEPT:
 *   Generated bodies share immutable code and independent execution domains.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "child native consumer FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

XR_DATA const XrXirProgramSpec child_callable_native_program;
XR_DATA const XrXirCallEntry child_callable_native_entries[];
#include "xir_child_callable_native_oracles.h"

int main(void) {
    _Static_assert(XR_XIR_CHECKED_CONTRACT == 70u, "current callable semantic contract");
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    CnFixture fixture = {0};
    cn_build(&fixture);
    cn_new_pair(&fixture);
    cn_run_pair(&fixture);
    cn_close(&fixture);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    puts("NATIVE_CHILD_LOCAL ordinaryNone=2 capturedNone=2 rootUnknownMixedDenied=8 crossInstanceDenied=2 forgedDenied=6 typed42=2 bytes42=2 Instances=2 compilerRuntimePhysical=0/0 mixedFIresources=OPEN");
    return 0;
}
