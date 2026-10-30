/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_rotation_cases.h - Checked callback rotation and real pipeline boundaries
 *
 * KEY CONCEPT:
 *   Five environments of one recursive body reach a genuine module Cell read.
 *   The five-function module has a shortest executing cause of seven steps.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include <string.h>

#include "xir_invocation_rotation_fixture.h"

/* The old public formula retains the Fn8 advertisements. The new private
 * equation certificate derives executing origins without publishing them. */
static XrXirStatus invocation_rotation_derive(const XrXirCompileContext *context,
    const XrXirModule *module,EffectInvocationCertificate **output) {
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectFinalFunctions *receiving=NULL;
    XrXirStatus status=invocation_core_storage(context,module,&effects);
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) {
        EffectBodyRequest owned_input={0};
        status=effect_invocation_collect(context,module,&effects,&graph,bounds,&owned_input);
        if (status==XR_XIR_OK) status=effect_invocation_current_input_match(context,module,graph.material);
        EffectInvocationDriverInput input={context,&owned_input,&effects,&graph,graph.material->declared,NULL};
        if (status==XR_XIR_OK) status=effect_invocation_derive(&input,output,NULL,&receiving);
    }
    effect_invocation_result_free(receiving);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);return status;
}

static XrXirStatus invocation_rotation_masks(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,bool oracle) {
    static const uint32_t expected[5]={XR_XIR_CALLABLE_ROOT_REQUIRED,
        XR_XIR_CALLABLE_ROOT_UNRESOLVED,0,XR_XIR_CALLABLE_ROOT_REQUIRED,XR_XIR_CALLABLE_ROOT_REQUIRED};
    if (!certificate || effect_body_count(certificate->body)!=5 || !certificate->final ||
        certificate->final->summary.count!=5) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<5 && status==XR_XIR_OK;++f) {
        uint32_t mask=UINT32_MAX;
        status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->final->roots[f],&mask);
        if (status==XR_XIR_OK && oracle) {
            CHECK(mask==expected[f]);
            const XrXirRootFormula *formula=&certificate->final->summary.functions[f].formula;
            CHECK(formula->constant_mask==(f==1?0:expected[f]));
            CHECK(formula->term_count==(f==1?5u:0u));
            for (uint32_t p=0;p<formula->term_count;++p)
                CHECK(formula->terms[p].kind==XR_XIR_ROOT_TERM_PARAMETER && formula->terms[p].index==p);
        }
    }
    uint32_t mask=UINT32_MAX;
    if (status==XR_XIR_OK)
        status=effect_invocation_certificate_edge_mask(context,certificate,0,2,1,&mask);
    if (status==XR_XIR_OK && oracle) CHECK(mask==XR_XIR_CALLABLE_ROOT_REQUIRED);
    return status;
}

/* The environment is part of the same owned certificate, not a hand-drawn
 * graph. Only the actual run, four rotations, invocation and slot-read edges
 * may form this chain; constructing the root reference is a latent edge. */
static void invocation_rotation_trace_oracle(const EffectInvocationCertificate *certificate,
    const EffectInvocationCauseTrace *trace) {
    static const uint32_t bodies[7]={0,1,1,1,1,1,3};
    static const uint32_t instructions[7]={2,1,1,1,1,0,0};
    CHECK(certificate->body->stage==XR_XIR_CHECKED && effect_body_count(certificate->body)==5);
    CHECK(trace && trace->facts.requires_root && !trace->facts.unresolved &&
        trace->counts[0]==7 && !trace->counts[1] && trace->counts[0]>effect_body_count(certificate->body));
    const EffectInvocationFinal *equations=certificate->final;
    CHECK(equations && equations->site_count==2 && !equations->binding_count);
    CHECK(equations->sites[0].function==0 && !equations->sites[0].instruction &&
        equations->sites[0].target==2 && equations->sites[1].function==0 &&
        equations->sites[1].instruction==1 && equations->sites[1].target==3);
    CHECK(equations->root_spaces && !equations->root_spaces[0] &&
        !certificate->body->functions[0].parameter_count);
    /* Two sites, zero bindings and zero namespace parameters give one Fn
     * word and one Cell word. No construction-only API survives sealing. */
    const uint32_t row_words=2;
    for (uint32_t k=0;k<7;++k) {
        const EffectInvocationTraceStep *step=&trace->steps[k];
        CHECK(step->function==bodies[k] && step->witness.instruction==instructions[k] &&
            step->witness.distance==6-k && step->witness.callee==(k==6?UINT32_MAX:bodies[k+1]) &&
            step->witness.slot==(k==6?0u:UINT32_MAX) &&
            step->witness.cause==(k==6?XR_XIR_ROOT_CAUSE_MUTABLE_SLOT:XR_XIR_ROOT_CAUSE_CALL));
        CHECK(step->equation<equations->count && step->root==equations->root_spaces[0]);
        const EffectInvocationNode *node=&equations->nodes[step->equation];
        CHECK(node->body==bodies[k] && node->root==step->root);
        if (!k || k==6) continue;
        CHECK(node->input && node->parameter_count==5);
        for (uint32_t p=0;p<5;++p) {
            CHECK(node->input[(size_t)p*row_words]==(UINT64_C(1)<<(p==5-k?1:0)));
            CHECK(!node->input[(size_t)p*row_words+1]);
        }
        for (uint32_t prior=1;prior<k;++prior) CHECK(trace->steps[prior].equation!=step->equation);
    }
}

typedef struct InvocationRotationStages {
    XrXirDiagnostic diagnostic;
    uint32_t phase,checked_count,specialized_count,lowered_count,public_forest_count;
    XrXirArtifact *checked,*specialized,*lowered;
} InvocationRotationStages;

static XrXirStatus invocation_rotation_pipeline(InvocationRotationStages *run) {
    run->phase=5;
    XrXirStatus status=xr_xir_compile_specialize(run->checked,&run->specialized,&run->diagnostic, NULL);
    if (status==XR_XIR_OK) {
        run->specialized_count=xr_xir_compile_artifact_module(run->specialized)->function_count;
        run->phase=6;status=xr_xir_compile_artifact_verify(run->specialized,&run->diagnostic);
    }
    if (status==XR_XIR_OK) {
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        run->phase=7;status=xr_xir_compile_lower(run->specialized,&target, NULL,&run->lowered,&run->diagnostic);
    }
    if (status==XR_XIR_OK) {
        run->lowered_count=xr_xir_compile_artifact_module(run->lowered)->function_count;
        run->phase=8;status=xr_xir_compile_artifact_verify(run->lowered,&run->diagnostic);
    }
    return status;
}

/* A focused pipeline run keeps the original finite allocator and caps. Its
 * actual failure is returned unchanged, including an unreachable lowering.
 * The ordinary/resource case ends at the fully verified Checked certificate. */
static XrXirStatus conditional_invocation_rotation(const XrXirCompileContext *context,
    bool pipeline,bool oracle) {
    XrCompileResourceStats begin=rp_stats(context),admitted={0};
    InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
    InvocationRotationStages run={0};EffectInvocationCertificate *certificate=NULL;
    XrXirEffects *public_effects=NULL;XrXirRootCauseTrace *public_trace=NULL;
    EffectInvocationCauseTrace *trace=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&run.checked,&run.diagnostic);
    admitted=rp_stats(context);
    if (status==XR_XIR_OK) {
        run.checked_count=xr_xir_compile_artifact_module(run.checked)->function_count;
        run.phase=1;status=xr_xir_compile_artifact_verify(run.checked,&run.diagnostic);
    }
    if (status==XR_XIR_OK) {
        run.phase=2;status=xr_xir_compile_effects_analyze(run.checked,&public_effects);
    }
    if (status==XR_XIR_OK) {
        run.public_forest_count=effect_contexts_read(public_effects) ? effect_contexts_read(public_effects)->forest->count : public_effects->count;
        run.phase=3;status=xr_xir_compile_root_cause_trace_copy(context,public_effects,0,&public_trace);
    }
    if (status==XR_XIR_OK) {
        run.phase=4;status=invocation_rotation_derive(context,xr_xir_compile_artifact_module(run.checked),&certificate);
    }
    if (status==XR_XIR_OK && pipeline) status=invocation_rotation_pipeline(&run);
    xr_xir_compile_effects_free(public_effects);
    xr_xir_compile_artifact_free(run.lowered);xr_xir_compile_artifact_free(run.specialized);
    xr_xir_compile_artifact_free(run.checked);memset(&fixture,0xa5,sizeof(fixture));
    if (status==XR_XIR_OK) {
        run.phase=9;status=invocation_rotation_masks(context,certificate,oracle);
    }
    if (status==XR_XIR_OK) {
        run.phase=10;status=effect_invocation_certificate_trace(context,certificate,0,&trace);
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(run.checked_count==5);invocation_rotation_trace_oracle(certificate,trace);
        uint32_t count=0;
        const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(public_trace,false,&count);
        static const uint32_t bodies[7]={0,1,1,1,1,1,3};
        static const uint32_t instructions[7]={2,1,1,1,1,0,0};
        CHECK(count==7 && count>run.checked_count && steps &&
            xr_xir_root_cause_trace_facts(public_trace)->requires_root &&
            !xr_xir_root_cause_trace_facts(public_trace)->unresolved);
        for (uint32_t k=0;k<7;++k) CHECK(steps[k].function==bodies[k] &&
            steps[k].instruction==instructions[k] && steps[k].distance==6-k &&
            steps[k].callee==(k==6?UINT32_MAX:bodies[k+1]) &&
            steps[k].slot==(k==6?0u:UINT32_MAX) &&
            steps[k].cause==(k==6?XR_XIR_ROOT_CAUSE_MUTABLE_SLOT:XR_XIR_ROOT_CAUSE_CALL));
    }
    if (oracle) {
        uint32_t root_count=0,unknown_count=0;
        bool has_diagnostic=run.phase<=1 || (run.phase>=5 && run.phase<=8);
        const XrXirRootEffects *public_facts=xr_xir_root_cause_trace_facts(public_trace);
        (void)xr_xir_root_cause_trace_steps(public_trace,false,&root_count);
        (void)xr_xir_root_cause_trace_steps(public_trace,true,&unknown_count);
        fprintf(stderr,"INVOCATION_ROTATION pipeline=%u phase=%u status=%u diagnostic=%u f=%u b=%u i=%u reason=%u "
            "checked=%u specialized=%u lowered=%u public_owner=%u public_requires=%u public_unresolved=%u "
            "public_root=%u public_unknown=%u "
            "private_root=%u private_unknown=%u\n",pipeline,run.phase,status,
            has_diagnostic,has_diagnostic?run.diagnostic.function:UINT32_MAX,
            has_diagnostic?run.diagnostic.block:UINT32_MAX,has_diagnostic?run.diagnostic.instruction:UINT32_MAX,
            has_diagnostic?run.diagnostic.reason:XR_XIR_DIAGNOSTIC_NONE,run.checked_count,
            run.specialized_count,run.lowered_count,run.public_forest_count,
            public_facts?public_facts->requires_root:0,public_facts?public_facts->unresolved:0,root_count,unknown_count,
            trace?trace->counts[0]:0,trace?trace->counts[1]:0);
        XrCompileResourceStats end=rp_stats(context);
        fprintf(stderr,"INVOCATION_ROTATION_RESOURCES allocated=%llu/%llu/%llu work=%llu/%llu/%llu "
            "live=%llu peak=%llu\n",(unsigned long long)begin.allocated_bytes,
            (unsigned long long)admitted.allocated_bytes,(unsigned long long)end.allocated_bytes,
            (unsigned long long)begin.work,(unsigned long long)admitted.work,(unsigned long long)end.work,
            (unsigned long long)end.live_bytes,(unsigned long long)end.peak_bytes);
    }
    effect_invocation_certificate_free(certificate);
    if (status==XR_XIR_OK && oracle) CHECK(trace && trace->counts[0]==7 &&
        trace->facts.requires_root && !trace->facts.unresolved && trace->steps[6].function==3);
    effect_invocation_trace_free(trace);xr_xir_compile_root_cause_trace_free(public_trace);return status;
}

static void conditional_invocation_rotation_run(bool pipeline) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirStatus status=conditional_invocation_rotation(&context,pipeline,true);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(status==XR_XIR_OK);
}
