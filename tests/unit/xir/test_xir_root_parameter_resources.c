/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_resources.c - Owned evidence failure and lifetime gates
 */
#include "xir_construction_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition); abort(); } } while (0)
#include "xir_root_parameter_fixture.h"

static const XrXirProvenance *rp_input(const RootParameterFixture *fixture, bool instance) {
    return instance ? &fixture->instance : &fixture->evidence;
}
static void rp_literals(const XrXirProvenance *copy, bool instance) {
    const XrXirProvenance *definition = copy;
    if (instance) {
        CHECK(copy->kind == 2 && copy->count == 1 && copy->source && copy->origins);
        CHECK(copy->origins[0].function == 0 && copy->origins[0].argument_count == 0 &&
            !copy->origins[0].arguments && copy->origins[0].effect_argument_count == 1);
        CHECK(copy->origins[0].effect_arguments[0].parameter == 0 &&
            copy->origins[0].effect_arguments[0].type == (XrXirType)256);
        definition = copy->source->module.provenance;
    }
    CHECK(definition && definition->kind == 1 && definition->contract_count == 1 && !definition->source &&
        !definition->origins && definition->count == 0);
    const XrXirFunctionEffectContract *contract = &definition->contracts[0];
    CHECK(contract->parameter_count == 1 && contract->parameters[0].kind == 1 && contract->parameters[0].uses == 1);
    CHECK(contract->formula.constant_mask == 0 && contract->formula.term_count == 1 &&
        contract->formula.terms[0].kind == 1 && contract->formula.terms[0].index == 0);
}
static void rp_lifetime(bool instance) {
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    RootParameterFixture fixture;
    rp_fixture(&fixture,&context);
    XrXirProvenance *copy = NULL;
    CHECK(xr_xir_compile_provenance_copy(&context,rp_input(&fixture,instance),&copy) == XR_XIR_OK);
    CHECK(copy != rp_input(&fixture,instance));
    const XrXirProvenance *definition = instance ? copy->source->module.provenance : copy;
    CHECK(definition->contracts != &fixture.contract && definition->contracts[0].parameters != &fixture.effect_parameter &&
        definition->contracts[0].formula.terms != &fixture.term);
    if (instance) CHECK(copy->source != &fixture.source && copy->origins != &fixture.origin &&
        copy->origins[0].effect_arguments != &fixture.effect_argument);
    memset(&fixture,0xa5,sizeof(fixture));
    rp_literals(copy,instance);
    xr_xir_compile_provenance_free(copy);
    rp_owner_free(&context,baseline);
    rp_balanced((RootParameterMark){0,0});
}
static void rp_faults(bool instance) {
    XrXirCompileContext normal = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&normal).live_bytes;
    RootParameterFixture fixture;
    rp_fixture(&fixture,&normal);
    XrXirProvenance *copy = NULL;
    rp_attempts = 0;
    CHECK(xr_xir_compile_provenance_copy(&normal,rp_input(&fixture,instance),&copy) == XR_XIR_OK);
    size_t sites = rp_attempts;
    CHECK(sites != 0);
    xr_xir_compile_provenance_free(copy);
    rp_owner_free(&normal,baseline);
    for (size_t fault = 0; fault < sites; ++fault) {
        XrXirCompileContext context = rp_owner(rp_caps());
        baseline = rp_stats(&context).live_bytes;
        rp_fixture(&fixture,&context);
        RootParameterMark mark = rp_mark();
        rp_attempts = 0; rp_fail_at = fault; rp_injected = false; copy = NULL;
        CHECK(xr_xir_compile_provenance_copy(&context,rp_input(&fixture,instance),&copy) == XR_XIR_OUT_OF_MEMORY);
        CHECK(rp_injected && !copy);
        rp_balanced(mark);
        XrCompileResourceStats failed = rp_stats(&context);
        rp_fail_at = SIZE_MAX;
        CHECK(xr_xir_compile_provenance_copy(&context,rp_input(&fixture,instance),&copy) == XR_XIR_OK);
        CHECK(rp_stats(&context).work >= failed.work && rp_stats(&context).allocated_bytes >= failed.allocated_bytes);
        rp_literals(copy,instance);
        xr_xir_compile_provenance_free(copy);
        rp_balanced(mark);
        rp_owner_free(&context,baseline);
        rp_balanced((RootParameterMark){0,0});
    }
    printf("owned root parameters instance=%u actual copy OOM sites=%zu\n",(unsigned)instance,sites);
}
static void rp_owner_death(bool instance) {
    XrXirCompileContext context = rp_owner(rp_caps());
    RootParameterFixture fixture;
    rp_fixture(&fixture,&context);
    XrXirProvenance *copy = NULL;
    CHECK(xr_xir_compile_provenance_copy(&context,rp_input(&fixture,instance),&copy) == XR_XIR_OK);
    memset(&fixture,0xa5,sizeof(fixture));
    xr_compile_resources_release(context.resources);
    memset(&context,0xa5,sizeof(context));
    rp_literals(copy,instance);
    if (instance) CHECK(xr_xir_compile_artifact_verify(copy->source,NULL) == XR_XIR_OK);
    xr_xir_compile_provenance_free(copy);
    rp_balanced((RootParameterMark){0,0});
}
static void rp_axes(bool instance) {
    XrXirCompileContext normal = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&normal).live_bytes;
    RootParameterFixture fixture;
    rp_fixture(&fixture,&normal);
    XrXirProvenance *copy = NULL;
    CHECK(xr_xir_compile_provenance_copy(&normal,rp_input(&fixture,instance),&copy) == XR_XIR_OK);
    XrCompileResourceStats required = rp_stats(&normal);
    xr_xir_compile_provenance_free(copy);
    rp_owner_free(&normal,baseline);
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrCompileResourceLimits caps = rp_caps();
        if (axis == 0) caps.allocated_bytes = required.allocated_bytes - shortfall;
        if (axis == 1) caps.live_bytes = required.peak_bytes - shortfall;
        if (axis == 2) caps.work = required.work - shortfall;
        XrXirCompileContext context = rp_owner(caps);
        baseline = rp_stats(&context).live_bytes;
        RootParameterMark mark = rp_mark();
        rp_fixture(&fixture,&context); copy = NULL;
        CHECK(xr_xir_compile_provenance_copy(&context,rp_input(&fixture,instance),&copy) ==
            (shortfall ? XR_XIR_BUDGET : XR_XIR_OK));
        if (shortfall) CHECK(!copy);
        else rp_literals(copy,instance);
        xr_xir_compile_provenance_free(copy);
        rp_balanced(mark);
        rp_owner_free(&context,baseline);
        rp_balanced((RootParameterMark){0,0});
    }
}
static void rp_early_failures(void) {
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    RootParameterFixture fixture;
    rp_fixture(&fixture,&context);
    XrXirProvenance *sentinel = (XrXirProvenance *)(uintptr_t)1;
    XrCompileResourceStats before = rp_stats(&context);
    CHECK(xr_xir_compile_provenance_copy(&context,&fixture.evidence,&sentinel) == XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel == (XrXirProvenance *)(uintptr_t)1 && rp_stats(&context).work == before.work &&
        rp_stats(&context).allocation_count == before.allocation_count);
    XrXirCompileContext invalid = {0};
    CHECK(xr_xir_compile_provenance_copy(&invalid,&fixture.evidence,&sentinel) == XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel == (XrXirProvenance *)(uintptr_t)1);
    for (unsigned bad = 0; bad < 4; ++bad) {
        rp_fixture(&fixture,&context);
        if (bad == 0) fixture.evidence.kind = 0;
        if (bad == 1) fixture.evidence.kind = UINT32_MAX;
        if (bad == 2) fixture.evidence.contracts = NULL;
        if (bad == 3) fixture.evidence.source = &fixture.source;
        XrXirProvenance *copy = NULL;
        RootParameterMark mark = rp_mark();
        CHECK(xr_xir_compile_provenance_copy(&context,&fixture.evidence,&copy) == XR_XIR_BAD_STRUCTURE);
        CHECK(!copy); rp_balanced(mark);
    }
    rp_fixture(&fixture,&context);
    fixture.source.module.provenance = &fixture.instance;
    XrXirProvenance *copy = NULL;
    CHECK(xr_xir_compile_provenance_copy(&context,&fixture.instance,&copy) == XR_XIR_BAD_STRUCTURE && !copy);
    rp_fixture(&fixture,&context);
    fixture.evidence.contract_count = UINT32_MAX;
    RootParameterMark mark = rp_mark();
    CHECK(xr_xir_compile_provenance_copy(&context,&fixture.evidence,&copy) == XR_XIR_BUDGET && !copy);
    rp_balanced(mark);
    rp_owner_free(&context,baseline);
    rp_balanced((RootParameterMark){0,0});
}

static XrXirStatus rp_trace_pipeline(const XrXirCompileContext *c, XrXirRootCauseTrace **output) {
    RootParameterFixture f;rp_fixture(&f,c);f.callable.flags=12;f.module.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&f,0xa5,sizeof(f));
    if (status==XR_XIR_OK) status=xr_xir_compile_root_cause_trace_copy(c,effects,0,&trace);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK) { *output=trace;trace=NULL; }
    xr_xir_compile_root_cause_trace_free(trace);return status;
}
static void rp_trace_literal(const XrXirRootCauseTrace *trace) {
    const XrXirRootEffects *facts=xr_xir_root_cause_trace_facts(trace);
    CHECK(facts && facts->requires_root && facts->unresolved);
    for (unsigned unknown=0;unknown<2;++unknown) {
        uint32_t count=0;const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,unknown!=0,&count);
        CHECK(count==1 && step && step[0].function==0 && step[0].instruction==0 && step[0].cause==8 &&
            step[0].callee==UINT32_MAX && step[0].slot==0 && step[0].distance==0);
    }
}
static void rp_trace_resources(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&normal).live_bytes;XrXirRootCauseTrace *trace=NULL;rp_attempts=0;
    CHECK(rp_trace_pipeline(&normal,&trace)==XR_XIR_OK && trace);rp_trace_literal(trace);
    size_t sites=rp_attempts;XrCompileResourceStats required=rp_stats(&normal);CHECK(sites);
    xr_xir_compile_root_cause_trace_free(trace);rp_owner_free(&normal,baseline);rp_balanced(physical);
    for (size_t i=0;i<sites;++i) {
        XrXirCompileContext c=rp_owner(rp_caps());baseline=rp_stats(&c).live_bytes;
        RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=i;rp_injected=false;trace=NULL;
        CHECK(rp_trace_pipeline(&c,&trace)==XR_XIR_OUT_OF_MEMORY && rp_injected && !trace);rp_balanced(mark);
        XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
        CHECK(rp_trace_pipeline(&c,&trace)==XR_XIR_OK && trace);rp_trace_literal(trace);
        CHECK(rp_stats(&c).work>=failed.work && rp_stats(&c).allocated_bytes>=failed.allocated_bytes);
        xr_xir_compile_root_cause_trace_free(trace);rp_owner_free(&c,baseline);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits caps=rp_caps();
        if (axis==0) caps.allocated_bytes=required.allocated_bytes-less;
        if (axis==1) caps.live_bytes=required.peak_bytes-less;
        if (axis==2) caps.work=required.work-less;
        XrXirCompileContext c=rp_owner(caps);baseline=rp_stats(&c).live_bytes;trace=NULL;
        CHECK(rp_trace_pipeline(&c,&trace)==(less?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(less?!trace:trace!=NULL);if (trace) rp_trace_literal(trace);
        xr_xir_compile_root_cause_trace_free(trace);rp_owner_free(&c,baseline);rp_balanced(physical);
    }
    printf("full parameter trace owner Check/effects/copy actual OOM sites=%zu\n",sites);
}
int main(void) {
    rp_trace_resources();
    rp_lifetime(false); rp_lifetime(true);
    rp_owner_death(false); rp_owner_death(true);
    rp_faults(false); rp_faults(true);
    rp_axes(false); rp_axes(true);
    rp_early_failures();
    puts("owned parameter evidence resource gates passed");
    return 0;
}
