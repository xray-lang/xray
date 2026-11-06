/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_outcome_cases.h - Complete publication after the last allocator
 */
#include "xir_invocation_rotation_fixture.h"

static XrXirEffects *pc_outcome_candidate;
static XrXirInstruction *pc_outcome_row;
static XrXirRootEffects *pc_outcome_root_allocation;
static XrXirRootEffectWitness *pc_outcome_root_witness_allocation,*pc_outcome_unknown_witness_allocation;
static uint32_t pc_outcome_mutation,pc_outcome_callbacks;

static void pc_outcome_late_allocate(void *memory) {
    size_t bytes=pc_final_physical_size(memory);
    if (!pc_outcome_candidate) {
        if (bytes==sizeof(CompileAllocation)+sizeof(XrXirEffects))
            pc_outcome_candidate=(XrXirEffects *)((CompileAllocation *)memory+1);
        return;
    }
    /* The first callback only records an address. A later allocation follows
     * its complete initialization and real ROOT publication. */
    if (!pc_outcome_candidate->invocations) return;
    CHECK(pc_outcome_candidate->invocations->final && !pc_outcome_callbacks);
    pc_outcome_root_allocation=pc_outcome_candidate->outcomes_work.root;
    pc_outcome_root_witness_allocation=pc_outcome_candidate->outcomes_work.root_witnesses;
    pc_outcome_unknown_witness_allocation=pc_outcome_candidate->outcomes_work.unresolved_witnesses;
    CHECK(pc_outcome_root_allocation && pc_outcome_root_witness_allocation && pc_outcome_unknown_witness_allocation);
    /* Every receiving field must come from the actual construction records. */
    memset(pc_outcome_candidate->outcomes_owner,0xA5,sizeof(*pc_outcome_candidate->outcomes_owner));
    rp_allocation_observer=NULL;++pc_outcome_callbacks;
    if (pc_outcome_mutation) pc_outcome_row->type=XR_XIR_BOOL;
}

static XrXirStatus pc_outcome_late_operation(const XrXirCompileContext *context,
    uint32_t mutation,bool oracle) {
    CHECK(mutation<2 && !rp_allocation_observer && !pc_outcome_candidate && !pc_outcome_row);
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    XrXirModule candidate={0};XrXirFunction functions[3];XrXirInstruction rows[EE_MAX_OPS];
    if (status==XR_XIR_OK) {
        const XrXirModule *source=xr_xir_compile_artifact_module(checked);
        CHECK(source->function_count==3 && source->functions[1].instruction_count<=EE_MAX_OPS);
        candidate=*source;memcpy(functions,source->functions,sizeof(functions));
        memcpy(rows,functions[1].instructions,(size_t)functions[1].instruction_count*sizeof(*rows));
        candidate.functions=functions;functions[1].instructions=rows;
        CHECK(rows[4].type==XR_XIR_I64);
        pc_outcome_row=&rows[4];pc_outcome_mutation=mutation;pc_outcome_callbacks=0;
        rp_allocation_observer=pc_outcome_late_allocate;
        status=xr_xir_compile_effects_infer_verified_input(context,
            (XirEffectInput){&candidate,xr_xir_compile_artifact_construction(checked),NULL},&effects);
        rp_allocation_observer=NULL;pc_outcome_candidate=NULL;pc_outcome_row=NULL;
        if (oracle) fprintf(stderr,"late whole Input mutation%u callbacks%u status%u output%u\n",
            mutation,pc_outcome_callbacks,status,effects!=NULL);
        if (status==XR_XIR_OK || status==XR_XIR_BAD_STRUCTURE) {
            CHECK(pc_outcome_callbacks==1);
            if (mutation) CHECK(status==XR_XIR_BAD_STRUCTURE && !effects);
            else CHECK(status==XR_XIR_OK && effects);
            status=XR_XIR_OK;
        } else CHECK(status==XR_XIR_BUDGET || status==XR_XIR_MEMORY_LIMIT || status==XR_XIR_OUT_OF_MEMORY);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
    if (effects) {
        const EffectFinalOutcomes *owned=effect_final_outcomes(effects);
        CHECK(owned && owned->root==pc_outcome_root_allocation &&
            owned->root_witnesses==pc_outcome_root_witness_allocation &&
            owned->unresolved_witnesses==pc_outcome_unknown_witness_allocation &&
            effect_outcomes_empty(&effects->outcomes_work));
        pc_empty_golden(effects,0);
        CHECK(xr_xir_effects_read(effects,1).control.suspend==XR_XIR_EFFECT_NONE &&
            xr_xir_effects_read(effects,1).control.throws==XR_XIR_EFFECT_NONE &&
            xr_xir_effects_go_safe(effects,1)==XR_XIR_OK &&
            xr_xir_effects_task_creation(effects,1)==XR_XIR_EFFECT_NONE);
    }
    xr_xir_compile_effects_free(effects);
    pc_outcome_root_allocation=NULL;pc_outcome_root_witness_allocation=NULL;
    pc_outcome_unknown_witness_allocation=NULL;
    CHECK(!rp_allocation_observer && !pc_outcome_candidate && !pc_outcome_row);return status;
}

static void pc_outcome_late_literals(void) {
    for (uint32_t mutation=0;mutation<2;++mutation) {
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
        CHECK(pc_outcome_late_operation(&context,mutation,true)==XR_XIR_OK);
        CHECK(pc_outcome_late_operation(&context,0,false)==XR_XIR_OK);
        rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    }
}

static void pc_outcome_owner_literals(void) {
    XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    CHECK(final && final==effects->invocations->final->outcomes &&
        final->resources==context.resources && final->count==3 &&
        !effects->outcomes_owner && effect_outcomes_empty(&effects->outcomes_work));
    XrXirEffectFacts control_read=xr_xir_effects_read(effects,1);
    const XrXirFunctionEffects *control=(control_read.status==XR_XIR_OK?&control_read.control:NULL);
    CHECK(control && control->suspend==XR_XIR_EFFECT_NONE && control->throws==XR_XIR_EFFECT_NONE);
    const XrXirFunctionEffects *stored=&final->functions[1];
    XrXirEffectFacts detached=xr_xir_effects_read(effects,0);
    CHECK(detached.status==XR_XIR_OK && detached.root.requires_root && !detached.root.unresolved &&
        detached.root_witness.cause==XR_XIR_ROOT_CAUSE_INITIALIZER);
    XrXirRootEffects untouched={true,true};
    XrCompileResourceStats denied=rp_stats(&context);size_t denied_attempts=rp_attempts;
    CHECK(xir_effects_construction_root(&context,effects,0,&untouched)==XR_XIR_BAD_STRUCTURE &&
        untouched.requires_root && untouched.unresolved && conditional_memory_unchanged(&context,denied) &&
        rp_attempts==denied_attempts);

    EffectInvocationCertificate *again=effects->invocations;EffectGraph graph={0};
    XrCompileResourceStats published=rp_stats(&context);size_t published_attempts=rp_attempts;
    const XrXirModule *source=xr_xir_compile_artifact_module(checked);
    CHECK(effect_invocation_publication_commit(&context,effects,&again)==XR_XIR_BAD_STRUCTURE &&
        again==effects->invocations &&
        effect_invocation_publish(&context,source,effects,&graph,NULL,NULL)==XR_XIR_BAD_STRUCTURE &&
        effect_outcomes_publish(&context,source,effects)==XR_XIR_BAD_STRUCTURE &&
        conditional_memory_unchanged(&context,published) && rp_attempts==published_attempts &&
        effect_final_outcomes(effects)==final);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    pc_empty_golden(effects,0);CHECK(&effect_final_outcomes(effects)->functions[1]==stored &&
        xr_xir_effects_read(effects,1).control.suspend==control->suspend &&
        xr_xir_effects_read(effects,1).control.throws==control->throws);
    XrXirEffects incomplete={.resources=context.resources,.count=effects->count,.invocations=effects->invocations};
    incomplete.outcomes_work.functions=final->functions;
    XrCompileResourceStats before=rp_stats(&context);size_t attempts_before=rp_attempts;
    CHECK(!effect_final_outcomes(&incomplete) && !(xr_xir_effects_read(&incomplete,1).status==XR_XIR_OK) &&
        !(xr_xir_effects_read(&incomplete,1).status==XR_XIR_OK && xr_xir_effects_read(&incomplete,1).suspend_witness.cause!=0) &&
        xr_xir_effects_go_safe(&incomplete,1)==XR_XIR_BAD_STRUCTURE &&
        xr_xir_effects_task_creation(&incomplete,1)==XR_XIR_EFFECT_UNKNOWN);
    CHECK(conditional_memory_unchanged(&context,before) && rp_attempts==attempts_before);
    uint32_t count=effects->invocations->final->outcomes->count;
    effects->invocations->final->outcomes->count=count+1;
    CHECK(!effect_final_outcomes(effects) && !(xr_xir_effects_read(effects,1).status==XR_XIR_OK));
    effects->invocations->final->outcomes->count=count;pc_empty_golden(effects,0);
    xr_xir_compile_effects_free(effects);
    CHECK(detached.status==XR_XIR_OK && detached.root.requires_root && !detached.root.unresolved &&
        detached.control.suspend==XR_XIR_EFFECT_NONE && detached.control.throws==XR_XIR_EFFECT_NONE &&
        detached.root_witness.cause==XR_XIR_ROOT_CAUSE_INITIALIZER);
    rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
}


static void pc_outcome_retry(const XrXirCompileContext *context,uint32_t mutation,
    XrCompileResourceLimits caps,const PcCurrentInputMeasure *normal) {
    XrCompileResourceStats before=rp_stats(context);
    CHECK(before.held_bytes==normal->initial.held_bytes);
    bool fits=normal->paid.peak_bytes<=caps.memory_limit;
    CHECK(pc_outcome_late_operation(context,mutation,false)==(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT));
    XrCompileResourceStats after=rp_stats(context);
    CHECK(after.held_bytes==before.held_bytes && after.allocated_bytes>=before.allocated_bytes);
}

static void pc_outcome_resources(void) {
    for (uint32_t mutation=0;mutation<2;++mutation) {
        PcCurrentInputMeasure normal={0};
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();normal.initial=rp_stats(&context);
        rp_attempts=0;CHECK(pc_outcome_late_operation(&context,mutation,false)==XR_XIR_OK);
        normal.sites=rp_attempts;normal.paid=rp_stats(&context);rp_owner_free(&context,normal.initial.held_bytes);CHECK(!rp_live && !rp_live_bytes);
        for (size_t ordinal=0;ordinal<normal.sites;++ordinal) {
            context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=rp_stats(&context);
            XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_injected=false;rp_fail_at=ordinal;
            XrXirStatus status=pc_outcome_late_operation(&context,mutation,false);rp_fail_at=SIZE_MAX;
            CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY && context.resources==identity &&
                rp_stats(&context).held_bytes==before.held_bytes);
            pc_outcome_retry(&context,mutation,rp_caps(),&normal);rp_owner_free(&context,before.held_bytes);CHECK(!rp_live && !rp_live_bytes);
        }
        for (uint32_t minus=0;minus<2;++minus) {
            CHECK(normal.paid.peak_bytes>1);
            XrCompileResourceLimits caps={XR_COMPILE_MEMORY_FINITE,normal.paid.peak_bytes-minus,true};
            context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();
            XrCompileResourceStats before=rp_stats(&context);
            CHECK(pc_outcome_late_operation(&context,mutation,false)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
            pc_outcome_retry(&context,mutation,caps,&normal);
            rp_owner_free(&context,before.held_bytes);CHECK(!rp_live && !rp_live_bytes);
        }
        printf("whole Final late mutation%u complete sites%zu allocated%llu peak%llu all ordinals held exact/short same-owner physical0\n",
            mutation,normal.sites,
            (unsigned long long)normal.paid.allocated_bytes,(unsigned long long)normal.paid.peak_bytes);
    }
}
