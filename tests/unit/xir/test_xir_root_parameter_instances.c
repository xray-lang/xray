/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_instances.c - Actual physical callable specialization
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir_root_parameter_capture_instances.h"

static void ri_shape(const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==XR_XIR_CHECKED && m->function_count==5);
    const XrXirProvenance *p=m->provenance;
    CHECK(p && p->kind==2 && p->source && p->count==5 && p->binding_count==1);
    const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
    CHECK(source && source->provenance && source->provenance->kind==1 && source->function_count==4);
    CHECK(source->provenance->contracts[1].formula.term_count==1 &&
        source->provenance->contracts[1].formula.terms[0].kind==1 &&
        source->provenance->contracts[1].formula.terms[0].index==0);
    CHECK(p->origins[0].function==0 && p->origins[1].function==1 &&
        p->origins[2].function==2 && p->origins[3].function==3 && p->origins[4].function==1);
    CHECK(p->origins[1].effect_argument_count==1 && p->origins[4].effect_argument_count==1);
    CHECK(p->origins[1].effect_arguments[0].parameter==0 &&
        p->origins[4].effect_arguments[0].parameter==0);
    const XrXirTypeNode *declared=xr_xir_callable_signature(m->types,m->functions[1].parameters[0]);
    const XrXirTypeNode *actual=xr_xir_callable_signature(m->types,m->functions[4].parameters[0]);
    CHECK(declared && actual && declared->flags==8 && actual->flags==2);
    CHECK(m->functions[1].name_length==5 && !memcmp(m->functions[1].name,"apply",5));
    CHECK(m->functions[4].name_length>5 && !memcmp(m->functions[4].name,"apply",5));
    CHECK(p->origins[1].effect_arguments[0].type==m->functions[1].parameters[0] &&
        p->origins[4].effect_arguments[0].type==m->functions[4].parameters[0]);
    CHECK(p->bindings[0].family==1 && p->bindings[0].caller==0 && p->bindings[0].instruction==2 &&
        p->bindings[0].callee==4 && p->bindings[0].parameter==0 && p->bindings[0].actual_value==1);
    CHECK(m->functions[0].instructions[2].op==XR_XIR_CALL && m->functions[0].instructions[2].immediate==4);
    CHECK(m->functions[2].instructions[0].op==XR_XIR_CONST_INT && m->functions[2].instructions[0].immediate==7);
}

static XrXirStatus ri_pipeline(const XrXirCompileContext *context, XrXirArtifact **output) {
    RootParameterBindingFixture f;rp_binding_fixture(&f,context,false);
    XrXirArtifact *checked=NULL,*read=NULL,*instance=NULL;
    XrXirCheckedPacket packet={0};XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(context, &f.module, &checked, &d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(checked,&packet,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&instance,&d);
    if (status==XR_XIR_OK) { ri_shape(instance); *output=instance;instance=NULL; }
    xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(read);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    return status;
}

static void ri_roundtrip(void) {
    RootParameterMark physical=rp_mark();
    XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirArtifact *instance=NULL,*read=NULL,*lowered=NULL;
    XrXirDiagnostic d={0};XrXirCheckedPacket packet={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(ri_pipeline(&context,&instance)==XR_XIR_OK && instance);
    CHECK(xr_xir_compile_artifact_verify(instance,&d)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(instance,&packet,&d)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&read,&d)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(instance);instance=NULL;
    ri_shape(read);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(read,&effects)==XR_XIR_OK && effects);
    CHECK(!xr_xir_effects_root(effects,0)->requires_root && !xr_xir_effects_root(effects,0)->unresolved);
    CHECK(!xr_xir_effects_root(effects,1)->requires_root && xr_xir_effects_root(effects,1)->unresolved);
    CHECK(!xr_xir_effects_root(effects,4)->requires_root && !xr_xir_effects_root(effects,4)->unresolved);
    xr_xir_compile_effects_free(effects);
    CHECK(xr_xir_compile_lower(read,&target,&lowered,&d)==XR_XIR_OK && lowered);
    CHECK(xr_xir_compile_artifact_module(lowered)->stage==XR_XIR_LOWERED);
    CHECK(xr_xir_compile_artifact_verify(lowered,&d)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(read);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void ri_forged(void) {
    RootParameterMark physical=rp_mark();
    XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirArtifact *instance=NULL;CHECK(ri_pipeline(&context,&instance)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(instance);
    /* Wrong actual type is a type proof failure; orphan/nonmatching edge
     * certificates are rejected by the unique structural binding predicate. */
    static const XrXirStatus expected[5]={XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,
        XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE};
    for (unsigned bad=0;bad<5;++bad) {
        XrXirProvenance *copy=NULL;
        CHECK(xr_xir_compile_provenance_copy(&context,original->provenance,&copy)==XR_XIR_OK && copy);
        XrXirOrigin *origins=(XrXirOrigin *)copy->origins;
        XrXirEffectArgument *argument=(XrXirEffectArgument *)origins[4].effect_arguments;
        XrXirEffectBindingProof *proof=(XrXirEffectBindingProof *)copy->bindings;
        if (bad==0) argument->type=origins[1].effect_arguments[0].type;
        if (bad==1) argument->parameter=1;
        if (bad==2) proof->actual_value=0;
        if (bad==3) proof->callee=1;
        if (bad==4) proof->family=2;
        XrXirModule input=*original;input.provenance=copy;
        XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};
        XrXirStatus status=xir_fixture_recheck(&context, &input, &checked, &d);
        CHECK(status==expected[bad] && !checked);
        xr_xir_compile_provenance_free(copy);
    }
    xr_xir_compile_artifact_free(instance);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void ri_faults_axes(void) {
    RootParameterMark physical=rp_mark();
    XrXirCompileContext normal=rp_owner(rp_caps());uint64_t baseline=rp_stats(&normal).live_bytes;
    XrXirArtifact *instance=NULL;rp_attempts=0;
    CHECK(ri_pipeline(&normal,&instance)==XR_XIR_OK && instance);
    size_t sites=rp_attempts;CHECK(sites);
    XrCompileResourceStats required=rp_stats(&normal);
    xr_xir_compile_artifact_free(instance);rp_owner_free(&normal,baseline);rp_balanced(physical);
    for (size_t fault=0;fault<sites;++fault) {
        XrXirCompileContext context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=fault;rp_injected=false;instance=NULL;
        CHECK(ri_pipeline(&context,&instance)==XR_XIR_OUT_OF_MEMORY && rp_injected && !instance);
        rp_balanced(mark);XrCompileResourceStats failed=rp_stats(&context);rp_fail_at=SIZE_MAX;
        CHECK(ri_pipeline(&context,&instance)==XR_XIR_OK && instance);
        CHECK(rp_stats(&context).work>=failed.work && rp_stats(&context).allocated_bytes>=failed.allocated_bytes);
        xr_xir_compile_artifact_free(instance);rp_balanced(mark);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits caps=rp_caps();
        if (axis==0) caps.allocated_bytes=required.allocated_bytes-shortfall;
        if (axis==1) caps.live_bytes=required.peak_bytes-shortfall;
        if (axis==2) caps.work=required.work-shortfall;
        XrXirCompileContext context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;instance=NULL;
        CHECK(ri_pipeline(&context,&instance)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?!instance:instance!=NULL);xr_xir_compile_artifact_free(instance);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    printf("actual apply Check/write/read/specialize OOM sites=%zu\n",sites);
}

int main(void) {
    ri_roundtrip();ri_forged();ri_faults_axes();
    rci_cases_run();
    CHECK(!rp_live && !rp_live_bytes);
    return 0;
}
