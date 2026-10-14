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
static bool ctfe_resource_loop,ctfe_resource_phi,ctfe_resource_no_declarations;
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
#include "xir_ctfe_loop_fixture.h"
static XrXirCtfeRequest ctfe_resource_request(const CtfeFixture *fixture,XrXirValue *argument) {
    *argument=(XrXirValue){XR_XIR_BOOL,0,0};
    return ctfe_request(fixture,ctfe_resource_phi?"swap":ctfe_resource_loop?"loop":"answer",
        ctfe_resource_loop?argument:NULL,ctfe_resource_loop?1:0);
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
    ctfe_expect(&fixture, ctfe_request(&fixture, "mutable", NULL, 0), XR_XIR_CTFE_OK, XR_XIR_I64, 2);
    {
        XrXirCtfeRequest owned=ctfe_request(&fixture,"owned",NULL,0);XrXirValue value={0};
        XrCompileResourceStats baseline=ctfe_stats(&fixture);
        CHECK(xr_xir_compile_ctfe_leaf(&owned,&value)==XR_XIR_CTFE_OK);
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&value,&bytes,&length) && length==7 && !memcmp(bytes,"managed",7));
        xr_xir_value_drop(&value);CHECK(ctfe_stats(&fixture).live_bytes==baseline.live_bytes);
    }
    const char *rejected[] = {"caller", "io"};
    for (uint32_t r = 0; r < 2; ++r) ctfe_expect(&fixture, ctfe_request(&fixture, rejected[r], NULL,
        0), XR_XIR_CTFE_NOT_LEAF, XR_XIR_UNIT, 0);
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
static void ctfe_declarationless(void) {
    XrXirCompileContext context={0};CtfeFixture fixture={0};
    CHECK(xr_compile_resources_new(&ctfe_owner_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();ctfe_resource_no_declarations=true;
    CHECK(ctfe_phi_build(&context,&fixture,NULL)==XR_XIR_OK);fixture.owns_resources=true;
    CHECK(!xr_xir_compile_artifact_module(fixture.lowered)->declarations);
    XrXirCtfeRequest request=ctfe_request(&fixture,"swap",NULL,0);
    ctfe_expect(&fixture,request,XR_XIR_CTFE_NOT_LEAF,XR_XIR_UNIT,0);
    XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};XrXirValue output={0};
    XrCompileResourceStats before=ctfe_stats(&fixture);
    size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(!output.type && !output.reserved && !output.payload);
    CHECK(ctfe_stats(&fixture).live_bytes==before.live_bytes);
    CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
    ctfe_fixture_free(&fixture);ctfe_resource_no_declarations=false;
    fprintf(stderr,"CTFE verified Lowered without declaration authority rejects both leaf/call entry; Unit and live preserved; physical=0/0\n");
}
static void ctfe_loops(void) {
    CtfeFixture fixture={0};ctfe_fixture_new(&fixture);
    XrXirValue flag={XR_XIR_BOOL,0,0};
    XrXirCtfeRequest request=ctfe_request(&fixture,"loop",&flag,1);
    ctfe_expect(&fixture,request,XR_XIR_CTFE_OK,XR_XIR_I64,1);
    flag.payload=1;
    ctfe_expect(&fixture,request,XR_XIR_CTFE_STEP_LIMIT,XR_XIR_UNIT,0);
    request.limits.steps=32;
    ctfe_expect(&fixture,request,XR_XIR_CTFE_STEP_LIMIT,XR_XIR_UNIT,0);
    ctfe_fixture_free(&fixture);
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&ctfe_owner_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    CHECK(ctfe_phi_build(&context,&fixture,NULL)==XR_XIR_OK);fixture.owns_resources=true;
    request=ctfe_request(&fixture,"swap",NULL,0);
    ctfe_expect(&fixture,request,XR_XIR_CTFE_OK,XR_XIR_I64,21);
    /* Four entry steps, four six-step headers, three two-step bodies and four exit steps. */
    request.limits.steps=38;ctfe_expect(&fixture,request,XR_XIR_CTFE_OK,XR_XIR_I64,21);
    request.limits.steps=37;ctfe_expect(&fixture,request,XR_XIR_CTFE_STEP_LIMIT,XR_XIR_UNIT,0);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;
    XrXirInstruction *ops=(XrXirInstruction *)fixture.lowered->module.functions[request.function].instructions;
    uint32_t saved=ops[11].targets[0];ops[11].targets[0]=99;
    ctfe_expect(&fixture,request,XR_XIR_CTFE_BAD_ARTIFACT,XR_XIR_UNIT,0);ops[11].targets[0]=saved;
    uint32_t *operands=(uint32_t *)fixture.lowered->module.functions[request.function].operands;
    saved=operands[2];operands[2]=3;
    ctfe_expect(&fixture,request,XR_XIR_CTFE_BAD_ARTIFACT,XR_XIR_UNIT,0);operands[2]=saved;
    size_t start=source_fixture_compile_attempts;
    ctfe_expect(&fixture,request,XR_XIR_CTFE_OK,XR_XIR_I64,21);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);
    for (size_t site=0;site<sites;++site) {
        source_fixture_compile_injected=false;
        source_fixture_compile_fail_at=source_fixture_compile_attempts+site;
        ctfe_expect(&fixture,request,XR_XIR_CTFE_OUT_OF_MEMORY,XR_XIR_UNIT,0);
        CHECK(source_fixture_compile_injected);source_fixture_compile_fail_at=SIZE_MAX;
        ctfe_expect(&fixture,request,XR_XIR_CTFE_OK,XR_XIR_I64,21);
    }
    ctfe_fixture_free(&fixture);
    fprintf(stderr,"CTFE Source false-loop=1; infinite loop bounded at 65536/32; parallel PHI swap(3)=21; exact38/minus37; malformed target/predecessor; evaluator FI=%zu same-owner retry; physical=0/0\n",sites);
}
#include "xir_ctfe_calls_cases.h"
#include "xir_ctfe_calls_resources.h"
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--declarationless")) { ctfe_declarationless();return 0; }
    if (argc==2 && (!strcmp(argv[1],"--loops") || !strcmp(argv[1],"--loop-resources") || !strcmp(argv[1],"--phi-resources"))) {
        if (!strcmp(argv[1],"--loops")) ctfe_loops();
        else {
            ctfe_resource_loop=!strcmp(argv[1],"--loop-resources");
            ctfe_resource_phi=!strcmp(argv[1],"--phi-resources");
            fprintf(stderr,"CTFE complete resources: %s\n",ctfe_resource_phi?"parallel PHI backedges":"Source scalar loop");
            ctfe_whole_resources();
        }
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
    }
    if (argc==2 && (!strcmp(argv[1],"--calls") || !strcmp(argv[1],"--call-resources"))) {
        if (!strcmp(argv[1],"--calls")) ctfe_calls_semantics();
        else ctfe_calls_resources();
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        return 0;
    }
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if (argc==2) ctfe_whole_resources();
    else { ctfe_semantics(); ctfe_allocation_failures(); ctfe_work_exhaustion(); }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source Checked twice specialization recheck Lowered CTFE fixed41/bool/i8, failures, producer death, physical0 PASS");
    return 0;
}
