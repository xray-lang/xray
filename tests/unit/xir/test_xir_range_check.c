/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_range_check.c - Typed strict range and packet admission controls
 *
 * KEY CONCEPT:
 *   Independently framed bytes and fixed endpoint vectors check the same
 *   range instruction that source recipes emit before value publication.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_checked_range68_golden.h"
#include "xir_checked_range69_golden.h"
_Static_assert(XR_XIR_RANGE_CHECK == 148 && XR_XIR_GO == 146 && XR_XIR_TASK_AWAIT == 147,
    "The range instruction preserves existing operation ordinals");
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25 && XR_XIR_CHECKED_CONTRACT == 69,
    "The unchanged wire schema carries the range semantic revision");

typedef struct RangeView {
    XrXirType parameters[3];
    XrXirInstruction instructions[3];
    XrXirBlock block;
    uint32_t operands[4];
    XrXirFunction function;
    XrXirModule module;
} RangeView;

static void range_view(RangeView *view) {
    memset(view, 0, sizeof(*view));
    for (unsigned i = 0; i < 3; ++i) { view->parameters[i] = XR_XIR_I64; view->operands[i] = i; }
    view->instructions[0] = (XrXirInstruction) {XR_XIR_RANGE_CHECK, XR_XIR_UNIT, {0,3}, {0}, 0, {0}};
    view->instructions[1] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    view->block = (XrXirBlock) {0,2,0,0};
    view->function = (XrXirFunction) {"range",5,view->parameters,3,XR_XIR_I64,&view->block,1,
        view->instructions,2,view->operands,3};
    view->module = (XrXirModule) {.stage=XR_XIR_BUILT,.functions=&view->function,.function_count=1};
}

static XrXirCompileContext range_context(void) {
    XrXirCompileContext context = {0};
    XrCompileResourceLimits limits = {67108864,8388608,128000000};
    context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    return context;
}

static void range_context_free(XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources,&stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == instance_compile_bytes);
    xr_compile_resources_release(context->resources); context->resources = NULL;
    instance_compile_zero();
}

static void range_rejections(void) {
    for (unsigned c = 0; c < 11; ++c) {
        RangeView view; range_view(&view);
        XrXirStatus expected = XR_XIR_BAD_STRUCTURE;
        switch (c) {
        case 0: view.instructions[0].args[1] = 2; break;
        case 1: view.instructions[0].args[1] = 4; break;
        case 2: view.instructions[0].args[0] = UINT32_MAX; break;
        case 3: view.instructions[0].type = XR_XIR_BOOL; expected = XR_XIR_BAD_TYPE; break;
        case 4: view.parameters[1] = XR_XIR_BOOL; expected = XR_XIR_BAD_TYPE; break;
        case 5: view.instructions[0].targets[0] = 1; break;
        case 6: view.instructions[0].immediate = 1; break;
        case 7: view.instructions[0].type_arguments[1] = 1; break;
        case 8: view.operands[2] = UINT32_MAX; expected = XR_XIR_BAD_VALUE; break;
        case 9:
            view.instructions[2] = view.instructions[1];
            view.instructions[1] = (XrXirInstruction) {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},9,{0}};
            view.function.instruction_count = view.block.count = 3;
            view.operands[2] = 4; expected = XR_XIR_BAD_DOMINANCE; break;
        case 10:
            view.function.operand_count = 4; view.instructions[0].args[0] = 1;
            view.operands[1] = 0; view.operands[2] = 1; view.operands[3] = 2; break;
        }
        XrXirCompileContext context = range_context();
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_verify(&context,&view.module,&diagnostic);
        if (status != expected) fprintf(stderr,"range rejection %u: status=%u expected=%u\n",c,status,expected);
        CHECK(status == expected && diagnostic.status == expected);
        range_context_free(&context);
    }
}

static void range_execute(const XrXirArtifact *artifact) {
    static const struct { int64_t start, end, length; bool valid; } cases[] = {
        {0,0,0,true}, {0,3,3,true}, {1,2,3,true}, {3,3,3,true},
        {INT64_MAX,INT64_MAX,INT64_MAX,true}, {-1,0,3,false},
        {0,-1,3,false}, {2,1,3,false}, {0,4,3,false}, {4,4,3,false},
        {INT64_MIN,INT64_MAX,3,false}, {0,INT64_MAX,3,false}, {0,0,-1,false}
    };
    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c) {
        XrXirValue arguments[] = {{XR_XIR_I64,0,cases[c].start},
            {XR_XIR_I64,0,cases[c].end},{XR_XIR_I64,0,cases[c].length}}, output = {0};
        XrXirRunContext run = {.steps=100,.frame_limit=4096};
        XrXirRunStatus status = xr_xir_compile_vm_run(artifact,0,&run,arguments,3,&output);
        CHECK(status == (cases[c].valid ? XR_XIR_RUN_OK : XR_XIR_RUN_NUMERIC_RANGE));
        if (cases[c].valid) CHECK(output.type == XR_XIR_I64 && !output.reserved && output.payload == cases[c].start);
        else CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(!run.live_bytes && run.allocations == run.frees);
    }
    XrXirValue arguments[] = {{XR_XIR_I64,0,0},{XR_XIR_I64,0,1},{XR_XIR_I64,0,1}}, output = {0};
    XrXirRunContext run = {.steps=0,.frame_limit=4096};
    CHECK(xr_xir_compile_vm_run(artifact,0,&run,arguments,3,&output) == XR_XIR_RUN_STEP_LIMIT);
    CHECK(!run.live_bytes && run.allocations == run.frees && !output.type && !output.payload);
}

static size_t range_pipeline(size_t failure, bool execute) {
    instance_compile_attempts = 0; instance_compile_fail_at = failure; instance_compile_injected = false;
    XrCompileResourceLimits limits = {67108864,8388608,128000000};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits,&context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) {
        CHECK(resource == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources && instance_compile_injected);
        instance_compile_zero(); return instance_compile_attempts;
    }
    RangeView view; range_view(&view);
    XrXirArtifact *checked = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_check(&context,&view.module,&checked,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(checked,&packet,NULL);
    if (status == XR_XIR_OK) {
        CHECK(packet.length == sizeof(checked_range69_golden));
        CHECK(!memcmp(packet.bytes,checked_range69_golden,packet.length));
        status = xr_xir_compile_checked_read(&context,checked_range69_golden,sizeof(checked_range69_golden),&decoded,NULL);
    }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(decoded,&target,&lowered,NULL);
    if (failure == SIZE_MAX) { CHECK(status == XR_XIR_OK); if (execute) range_execute(lowered); }
    else {
        if (status != XR_XIR_OUT_OF_MEMORY || !instance_compile_injected)
            fprintf(stderr,"range compiler failure=%zu status=%u injected=%u attempts=%zu\n",
                failure,status,(unsigned)instance_compile_injected,instance_compile_attempts);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && instance_compile_injected);
    }
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(checked);
    size_t attempts = instance_compile_attempts;
    range_context_free(&context);
    return attempts;
}

static void range_old_packet(void) {
    XrXirCompileContext context = range_context();
    const size_t before = instance_compile_attempts;
    const uint8_t *packets[] = {checked_range67_rejected, checked_range68_rejected};
    const size_t lengths[] = {sizeof(checked_range67_rejected), sizeof(checked_range68_rejected)};
    CHECK(sizeof(checked_range68_rejected) == sizeof(checked_range68_golden) &&
        !memcmp(checked_range68_rejected, checked_range68_golden, sizeof(checked_range68_golden)));
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        XrXirArtifact *artifact = NULL;
        XrXirDiagnostic diagnostic = {0};
        CHECK(xr_xir_compile_checked_read(&context,packets[i],lengths[i],&artifact,&diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(!artifact && diagnostic.status == XR_XIR_BAD_STRUCTURE && instance_compile_attempts == before);
        artifact = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(&context,packets[i],lengths[i],&artifact,&diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(artifact == (XrXirArtifact *)(uintptr_t)1 && diagnostic.status == XR_XIR_BAD_STRUCTURE &&
            instance_compile_attempts == before);
    }
    range_context_free(&context);
}

int main(void) {
    range_rejections(); range_old_packet();
    const size_t sites = range_pipeline(SIZE_MAX,false);
    (void) range_pipeline(SIZE_MAX,true);
    for (size_t i = 0; i < sites; ++i) CHECK(range_pipeline(i,false) == i+1);
    instance_compile_fail_at = SIZE_MAX;
    instance_compile_report();
    printf("range vectors=13 rejections=11 complete compiler OOM=%zu old67/68=early-reject-zeroalloc-empty-occupied\n",sites);
    return 0;
}
