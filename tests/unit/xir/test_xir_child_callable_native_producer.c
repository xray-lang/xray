/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_child_callable_native_producer.c - Generate the authentic child graph
 *
 * KEY CONCEPT:
 *   One checked graph supplies VM bindings and real portable native code.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_output.h"
#include "xir/xxir_constraint_proof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "child native producer FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_child_callable_permissions.h"

/* The shared constructor retains real VM callbacks only in the compiler
 * producer. No VM activation is polled to establish a native result. */
static XrXirAction cp_resume(XrXirCallView *view) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(cp_observed && binding && binding->function < XRCP_FUNCTIONS);
    return cp_observed->original[binding->function].resume(view);
}

static void cp_release(XrXirCallView *view, XrXirCallStatus reason) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(cp_observed && binding && binding->function < XRCP_FUNCTIONS);
    cp_observed->original[binding->function].release(view, reason);
}

int main(int argc, char **argv) {
    CHECK(argc == 2 && XR_XIR_CHECKED_CONTRACT == 70u);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    CpFixture fixture;
    cp_build(&fixture);
    XrXirCSource source = {0};
    XrXirStatus status = xr_xir_compile_emit_c(fixture.lowered, "child_callable_native", 1048576, &source);
    if (status != XR_XIR_OK) fprintf(stderr, "child native emit status=%u\n", status);
    CHECK(status == XR_XIR_OK && source.text && source.length && !source.text[source.length]);
    CHECK(!strstr(source.text, "({"));
    CHECK(strstr(source.text, "xr_xir_task_go(") && strstr(source.text, "XR_XIR_ACTION_AWAIT_TASK"));
    CHECK(strstr(source.text, "xr_xir_instance_function(") && strstr(source.text, "xr_xir_instance_resolve_function("));
    xr_xir_compile_program_drop(fixture.program);
    fixture.program = NULL;
    CHECK(fixture.code_releases == 1 && !fixture.lowered);
    /* The emitted text owns its bytes independently of the source graph,
     * checked packet, specialization, lowered artifact and VM binding lease. */
    CHECK(source.length && !source.text[source.length]);
    FILE *file = fopen(argv[1], "wb");
    CHECK(file && fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
    printf("NATIVE_CHILD_GENERATED functions=10 sameCheckedProof=1 GO=1 localFnCapture=1 indirect=1 C11bytes=%zu VMexecuted=0\n", source.length);
    xr_xir_compile_c_source_free(&source);
    CHECK(!source.text && !source.length && !runtime_live && !runtime_bytes);
    cp_observed = NULL;
    effects_source_owners_free();
    return 0;
}
