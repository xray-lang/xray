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

typedef struct InvocationRotationFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;
    XrXirType parameters[5];uint32_t run_operands[5],rotate_operands[5];
    XrXirInstruction run[4],rotate[3],pure[2],root[3],init[4];
    XrXirBlock blocks[5];XrXirFunction functions[5];
    XrXirEffectParameter roles[5];XrXirRootTerm terms[5];
    XrXirRootValueIdentity values[2];XrXirFunctionEffectContract contracts[5];
    XrXirProvenance evidence;XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} InvocationRotationFixture;

static inline void invocation_rotation_fixture(InvocationRotationFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->run[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,5}};
    f->run[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->rotate[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    f->rotate[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,5}};
    f->rotate[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={6,0}};
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->root[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257,.immediate=0};
    f->root[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0,0}};
    f->root[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)257,.args={0,0}};
    f->init[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1,0},.immediate=0};
    f->init[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->run_operands[4]=1;
    for (uint32_t p=0;p<5;++p) {
        f->parameters[p]=(XrXirType)256;f->rotate_operands[p]=(p+1)%5;
        f->roles[p]=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,
            XR_XIR_EFFECT_USE_FORWARD|(p?0:XR_XIR_EFFECT_USE_INVOKE)};
        f->terms[p]=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,p};
    }
    f->blocks[0]=(XrXirBlock){.count=4};f->blocks[1]=(XrXirBlock){.count=3};
    f->blocks[2]=(XrXirBlock){.count=2};f->blocks[3]=(XrXirBlock){.count=3};
    f->blocks[4]=(XrXirBlock){.count=4};
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,
        .instructions=f->run,.instruction_count=4,.operands=f->run_operands,.operand_count=5};
    f->functions[1]=(XrXirFunction){.name="rotate",.name_length=6,.result=XR_XIR_I64,
        .parameters=f->parameters,.parameter_count=5,.instructions=f->rotate,.instruction_count=3,
        .operands=f->rotate_operands,.operand_count=5};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->pure,.instruction_count=2};
    f->functions[3]=(XrXirFunction){.name="root",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->root,.instruction_count=3};
    f->functions[4]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=f->init,.instruction_count=4};
    for (uint32_t i=0;i<5;++i) { f->functions[i].blocks=&f->blocks[i];f->functions[i].block_count=1; }
    f->values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->contracts[0]=(XrXirFunctionEffectContract){.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED},
        .values=f->values,.value_count=2};
    f->contracts[1]=(XrXirFunctionEffectContract){.parameter_count=5,.parameters=f->roles,
        .formula={.terms=f->terms,.term_count=5}};
    f->contracts[3].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[4].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=5};
    f->source=(XrXirSourceModule){.name="rotation",.name_length=8,.initializer=4};
    f->slot=(XrXirSlot){.type=(XrXirType)257,.mutable=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.slots=&f->slot,.slot_count=1,.entry_function=0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.provenance=&f->evidence,.linkage_kind=XR_XIR_PROGRAM};
}


/* The old public formula retains the Fn8 advertisements. The new private
 * equation certificate derives executing origins without publishing them. */
static XrXirStatus invocation_rotation_derive(const XrXirCompileContext *context,
    const XrXirModule *module,EffectInvocationCertificate **output) {
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    XrXirStatus status=invocation_core_storage(context,module,&effects);
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) {
        EffectInvocationDriverInput input={context,module,&effects,&graph,bounds,NULL};
        status=effect_invocation_derive(&input,output);
    }
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);return status;
}

static XrXirStatus invocation_rotation_masks(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,bool oracle) {
    static const uint32_t expected[5]={XR_XIR_CALLABLE_ROOT_REQUIRED,
        XR_XIR_CALLABLE_ROOT_UNRESOLVED,0,XR_XIR_CALLABLE_ROOT_REQUIRED,XR_XIR_CALLABLE_ROOT_REQUIRED};
    if (!certificate || certificate->bodies.function_count!=5 || !certificate->solution ||
        certificate->solution->count!=5) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<5 && status==XR_XIR_OK;++f) {
        uint32_t mask=UINT32_MAX;
        status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[f],&mask);
        if (status==XR_XIR_OK && oracle) {
            CHECK(mask==expected[f]);
            const XrXirRootFormula *formula=&certificate->solution->formulas[f];
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
    CHECK(certificate->bodies.stage==XR_XIR_CHECKED && certificate->bodies.function_count==5);
    CHECK(trace && trace->facts.requires_root && !trace->facts.unresolved &&
        trace->counts[0]==7 && !trace->counts[1] && trace->counts[0]>certificate->bodies.function_count);
    const EffectInvocationOwner *equations=certificate->equations;
    CHECK(equations && equations->site_count==2 && !equations->binding_count);
    CHECK(equations->sites[0].function==0 && !equations->sites[0].instruction &&
        equations->sites[0].target==2 && equations->sites[1].function==0 &&
        equations->sites[1].instruction==1 && equations->sites[1].target==3);
    CHECK(equations->root_spaces && !equations->root_spaces[0] &&
        !certificate->bodies.functions[0].parameter_count);
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
    XrXirStatus status=xr_xir_compile_specialize(run->checked,&run->specialized,&run->diagnostic);
    if (status==XR_XIR_OK) {
        run->specialized_count=xr_xir_compile_artifact_module(run->specialized)->function_count;
        run->phase=6;status=xr_xir_compile_artifact_verify(run->specialized,&run->diagnostic);
    }
    if (status==XR_XIR_OK) {
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        run->phase=7;status=xr_xir_compile_lower(run->specialized,&target,&run->lowered,&run->diagnostic);
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
        run.public_forest_count=public_effects->contexts ? public_effects->contexts->forest->count : public_effects->count;
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
        (void)xr_xir_root_cause_trace_steps(public_trace,false,&count);
        CHECK(count<=run.public_forest_count && xr_xir_root_cause_trace_facts(public_trace));
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
