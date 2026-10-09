/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_ctfe_leaf.c - Real Source scalar compiler evaluation boundaries
 *
 * KEY CONCEPT:
 *   Two owned packet round trips precede specialization and Lowered execution.
 *   Independent fixed answers and physical allocator failures carry the proof.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_internal.h"

static const char ctfe_source[] =
    "export fn answer()->i64 { return 6*7-1 }\n"
    "export fn choose(flag:bool,x:i64,y:i64)->i64 { return flag ? x+1 : y-2 }\n"
    "export fn boolean(value:bool)->bool { return value }\n"
    "export fn narrow(value:i8)->i8 { return value+1 }\n"
    "export fn quotient(a:i64,b:i64)->i64 { return a/b }\n"
    "export fn caller()->i64 { return answer() }\n"
    "export fn mutable()->i64 { var value=1; value=2; return value }\n"
    "export fn owned()->string { return \"managed\" }\n"
    "export fn io()->i64 { print(1); return 1 }\n"
    "export fn loop(flag:bool)->i64 { while(flag) {}; return 1 }\n";
static const XrCompileResourceLimits ctfe_owner_limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
typedef struct CtfeFixture {
    XrXirCompileContext context;
    XrXirArtifact *lowered;
    XrCompileResourceStats owner_baseline;
    uint8_t identity[32];
    bool owns_resources;
} CtfeFixture;
static XrCompileResourceStats ctfe_stats(const CtfeFixture *fixture) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(fixture->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
#include "xir_ctfe_leaf_chain.h"
static void ctfe_fixture_new(CtfeFixture *fixture) {
    CHECK(!fixture->context.resources && !fixture->lowered);
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&ctfe_owner_limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrXirStatus status = ctfe_fixture_build(&context, fixture, NULL);
    if (status != XR_XIR_OK) {
        fprintf(stderr, "Source CTFE producer status%u\n", (unsigned)status);
        xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    CHECK(status == XR_XIR_OK); fixture->owns_resources = true;
}
static void ctfe_fixture_free(CtfeFixture *fixture) {
    xr_xir_compile_artifact_free(fixture->lowered); fixture->lowered = NULL;
    XrCompileResourceStats stats = ctfe_stats(fixture);
    CHECK(stats.live_bytes == fixture->owner_baseline.live_bytes);
    CHECK(stats.allocated_bytes >= fixture->owner_baseline.allocated_bytes && stats.work >= fixture->owner_baseline.work);
    CHECK(stats.allocated_bytes <= ctfe_owner_limits.allocated_bytes && stats.peak_bytes <= ctfe_owner_limits.live_bytes &&
        stats.work <= ctfe_owner_limits.work);
    bool owns_resources = fixture->owns_resources;
    if (owns_resources) xr_compile_resources_release(fixture->context.resources);
    *fixture = (CtfeFixture){0};
    if (owns_resources) CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static XrXirCtfeRequest ctfe_request(const CtfeFixture *fixture, const char *name,
    const XrXirValue *arguments, uint32_t count) {
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    XrXirCtfeRequest request = {0}; request.artifact = fixture->lowered;
    request.function = UINT32_MAX; request.target = *xr_xir_compile_artifact_target(fixture->lowered);
    memcpy(request.checked_identity, fixture->identity, 32);
    request.arguments = arguments; request.argument_count = count;
    request.limits = (XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != strlen(name) || memcmp(function->name, name, function->name_length)) continue;
        CHECK(request.function == UINT32_MAX); request.function = f;
    }
    CHECK(request.function != UINT32_MAX); return request;
}
#include "xir_ctfe_leaf_resources.h"
static void ctfe_expect(CtfeFixture *fixture, XrXirCtfeRequest request,
    XrXirCtfeStatus expected, XrXirType type, int64_t payload) {
    XrCompileResourceStats before = ctfe_stats(fixture); XrXirValue result = {0};
    size_t live = source_fixture_compile_live, bytes = source_fixture_compile_bytes;
    XrXirCtfeStatus status = xr_xir_compile_ctfe_leaf(&request, &result);
    if (status != expected) fprintf(stderr, "CTFE function%u got%u want%u\n", request.function, (unsigned)status, (unsigned)expected);
    CHECK(status == expected);
    CHECK(result.type == (uint32_t)type && !result.reserved && result.payload == payload);
    XrCompileResourceStats after = ctfe_stats(fixture);
    CHECK(after.live_bytes == before.live_bytes && after.work >= before.work && after.allocated_bytes >= before.allocated_bytes);
    CHECK(source_fixture_compile_live == live && source_fixture_compile_bytes == bytes);
}
static void ctfe_semantics(void) {
    CtfeFixture fixture = {0}; ctfe_fixture_new(&fixture);
    ctfe_expect(&fixture, ctfe_request(&fixture, "answer", NULL, 0), XR_XIR_CTFE_OK, XR_XIR_I64, 41);
    XrXirValue args[] = {{XR_XIR_BOOL,0,1},{XR_XIR_I64,0,40},{XR_XIR_I64,0,9}};
    XrXirCtfeRequest choose = ctfe_request(&fixture, "choose", args, 3);
    const XrXirFunction *body = &xr_xir_compile_artifact_module(fixture.lowered)->functions[choose.function];
    bool phi = false; for (uint32_t i = 0; i < body->instruction_count; ++i) phi |= body->instructions[i].op == XR_XIR_PHI;
    CHECK(phi);
    ctfe_expect(&fixture, choose, XR_XIR_CTFE_OK, XR_XIR_I64, 41);
    args[0].payload = 0; ctfe_expect(&fixture, choose, XR_XIR_CTFE_OK, XR_XIR_I64, 7);
    ctfe_expect(&fixture, ctfe_request(&fixture, "boolean", args, 1), XR_XIR_CTFE_OK, XR_XIR_BOOL, 0);
    XrXirValue small = {XR_XIR_I8,0,126};
    ctfe_expect(&fixture, ctfe_request(&fixture, "narrow", &small, 1), XR_XIR_CTFE_OK, XR_XIR_I8, 127);
    small.payload = 127; ctfe_expect(&fixture, ctfe_request(&fixture, "narrow", &small, 1),
        XR_XIR_CTFE_OK, XR_XIR_I8, -128);
    small.payload = 128; ctfe_expect(&fixture, ctfe_request(&fixture, "narrow", &small, 1),
        XR_XIR_CTFE_BAD_ARGUMENT, XR_XIR_UNIT, 0);
    XrXirValue division[] = {{XR_XIR_I64,0,83},{XR_XIR_I64,0,2}};
    ctfe_expect(&fixture, ctfe_request(&fixture, "quotient", division, 2), XR_XIR_CTFE_OK, XR_XIR_I64, 41);
    division[1].payload = 0; ctfe_expect(&fixture, ctfe_request(&fixture, "quotient", division, 2),
        XR_XIR_CTFE_DIVIDE_BY_ZERO, XR_XIR_UNIT, 0);
    division[0].payload = INT64_MIN; division[1].payload = -1;
    ctfe_expect(&fixture, ctfe_request(&fixture, "quotient", division, 2), XR_XIR_CTFE_OK, XR_XIR_I64, INT64_MIN);
    const char *rejected[] = {"caller", "mutable", "owned", "io", "loop"};
    for (uint32_t r = 0; r < 5; ++r) ctfe_expect(&fixture, ctfe_request(&fixture, rejected[r], r == 4 ? args : NULL,
        r == 4 ? 1 : 0), XR_XIR_CTFE_NOT_LEAF, XR_XIR_UNIT, 0);
    XrXirCtfeRequest request = ctfe_request(&fixture, "answer", NULL, 0);
    XrXirValue occupied = {XR_XIR_I64,77,99}, original = occupied;
    XrCompileResourceStats before = ctfe_stats(&fixture);
    CHECK(xr_xir_compile_ctfe_leaf(&request, &occupied) == XR_XIR_CTFE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &original, sizeof(original)));
    XrCompileResourceStats after = ctfe_stats(&fixture); CHECK(!memcmp(&before, &after, sizeof(before)));
    request.target.abi_version ^= 1; ctfe_expect(&fixture, request, XR_XIR_CTFE_PROFILE, XR_XIR_UNIT, 0);
    request = ctfe_request(&fixture, "answer", NULL, 0); request.checked_identity[0] ^= 1;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_IDENTITY, XR_XIR_UNIT, 0);
    request = ctfe_request(&fixture, "answer", NULL, 0); request.limits.steps = 1;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_STEP_LIMIT, XR_XIR_UNIT, 0);
    request = ctfe_request(&fixture, "answer", NULL, 0); request.limits.frame_bytes = 0;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_FRAME_LIMIT, XR_XIR_UNIT, 0);
    request = ctfe_request(&fixture, "answer", NULL, 0); request.limits.output_bytes = 7;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_OUTPUT_LIMIT, XR_XIR_UNIT, 0);
    request = ctfe_request(&fixture, "boolean", args, 1); request.limits.output_bytes = 1;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_OK, XR_XIR_BOOL, 0);
    uint8_t saved = fixture.lowered->checked_identity[0]; fixture.lowered->checked_identity[0] ^= 1;
    request = ctfe_request(&fixture, "answer", NULL, 0);
    ctfe_expect(&fixture, request, XR_XIR_CTFE_BAD_ARTIFACT, XR_XIR_UNIT, 0);
    fixture.lowered->checked_identity[0] = saved;
    XrXirValue retained = {0}; request = ctfe_request(&fixture, "answer", NULL, 0);
    CHECK(xr_xir_compile_ctfe_leaf(&request, &retained) == XR_XIR_CTFE_OK);
    ctfe_fixture_free(&fixture);
    CHECK(retained.type == XR_XIR_I64 && !retained.reserved && retained.payload == 41);
}
static void ctfe_allocation_failures(void) {
    CtfeFixture fixture = {0}; ctfe_fixture_new(&fixture);
    XrXirCtfeRequest request = ctfe_request(&fixture, "answer", NULL, 0);
    size_t start = source_fixture_compile_attempts;
    ctfe_expect(&fixture, request, XR_XIR_CTFE_OK, XR_XIR_I64, 41);
    size_t sites = source_fixture_compile_attempts-start; CHECK(sites > 0);
    for (size_t site = 0; site < sites; ++site) {
        source_fixture_compile_injected = false;
        source_fixture_compile_fail_at = source_fixture_compile_attempts + site;
        ctfe_expect(&fixture, request, XR_XIR_CTFE_OUT_OF_MEMORY, XR_XIR_UNIT, 0);
        CHECK(source_fixture_compile_injected); source_fixture_compile_fail_at = SIZE_MAX;
        ctfe_expect(&fixture, request, XR_XIR_CTFE_OK, XR_XIR_I64, 41);
    }
    ctfe_fixture_free(&fixture);
    fprintf(stderr, "CTFE real compiler allocation failures=%zu; each preserves output/live baseline; physical=0/0\n", sites);
}
static void ctfe_work_exhaustion(void) {
    CtfeFixture fixture = {0}; ctfe_fixture_new(&fixture);
    XrXirCtfeRequest request = ctfe_request(&fixture, "answer", NULL, 0);
    XrCompileResourceStats before = ctfe_stats(&fixture);
    CHECK(xr_compile_resources_work(fixture.context.resources, ctfe_owner_limits.work-before.work-1) == XR_COMPILE_RESOURCE_OK);
    ctfe_expect(&fixture, request, XR_XIR_CTFE_BUDGET, XR_XIR_UNIT, 0);
    XrCompileResourceStats stopped = ctfe_stats(&fixture);
    CHECK(stopped.work >= ctfe_owner_limits.work-1);
    ctfe_expect(&fixture, request, XR_XIR_CTFE_BUDGET, XR_XIR_UNIT, 0);
    CHECK(ctfe_stats(&fixture).work == stopped.work);
    ctfe_fixture_free(&fixture);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if (argc==2) ctfe_whole_resources();
    else { ctfe_semantics(); ctfe_allocation_failures(); ctfe_work_exhaustion(); }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source Checked twice specialization recheck Lowered CTFE fixed41/bool/i8, failures, producer death, physical0 PASS");
    return 0;
}
