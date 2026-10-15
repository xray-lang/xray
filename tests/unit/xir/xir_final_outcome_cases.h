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
static uint32_t pc_outcome_mutation,pc_outcome_callbacks;

static void pc_outcome_late_allocate(void *memory,size_t bytes) {
    if (!pc_outcome_candidate) {
        if (bytes==sizeof(CompileAllocation)+sizeof(XrXirEffects))
            pc_outcome_candidate=(XrXirEffects *)((CompileAllocation *)memory+1);
        return;
    }
    /* The first callback only records an address. A later allocation follows
     * its complete initialization and real ROOT publication. */
    if (!pc_outcome_candidate->invocations) return;
    CHECK(pc_outcome_candidate->invocations->final && !pc_outcome_callbacks);
    pc_allocate_hook=NULL;++pc_outcome_callbacks;
    if (pc_outcome_mutation) pc_outcome_row->type=XR_XIR_BOOL;
}

static XrXirStatus pc_outcome_late_operation(const XrXirCompileContext *context,
    uint32_t mutation,bool oracle) {
    CHECK(mutation<2 && !pc_allocate_hook && !pc_outcome_candidate && !pc_outcome_row);
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
        pc_allocate_hook=pc_outcome_late_allocate;
        status=xr_xir_compile_effects_infer_verified(context,&candidate,&effects);
        pc_allocate_hook=NULL;pc_outcome_candidate=NULL;pc_outcome_row=NULL;
        if (oracle) fprintf(stderr,"late whole Input mutation%u callbacks%u status%u output%u\n",
            mutation,pc_outcome_callbacks,status,effects!=NULL);
        if (status==XR_XIR_OK || status==XR_XIR_BAD_STRUCTURE) {
            CHECK(pc_outcome_callbacks==1);
            if (mutation) CHECK(status==XR_XIR_BAD_STRUCTURE && !effects);
            else CHECK(status==XR_XIR_OK && effects);
            status=XR_XIR_OK;
        } else CHECK(status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
    if (effects) {
        pc_empty_golden(effects,0);
        CHECK(xr_xir_effects_function(effects,1)->suspend==XR_XIR_EFFECT_NONE &&
            xr_xir_effects_function(effects,1)->throws==XR_XIR_EFFECT_NONE &&
            xr_xir_effects_go_safe(effects,1)==XR_XIR_OK &&
            xr_xir_effects_task_errors(effects,1)==XR_XIR_OK &&
            xr_xir_effects_task_creation(effects,1)==XR_XIR_EFFECT_NONE &&
            xr_xir_effects_error(effects,2,(XrXirType)EE_ENUM,1));
    }
    xr_xir_compile_effects_free(effects);
    CHECK(!pc_allocate_hook && !pc_outcome_candidate && !pc_outcome_row);return status;
}

static void pc_outcome_late_literals(void) {
    for (uint32_t mutation=0;mutation<2;++mutation) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        CHECK(pc_outcome_late_operation(&context,mutation,true)==XR_XIR_OK);
        CHECK(pc_outcome_late_operation(&context,0,false)==XR_XIR_OK);
        pc_release(&context,baseline);
    }
}

static void pc_outcome_owner_literals(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    CHECK(final && final==effects->invocations->final->outcomes &&
        final->resources==context.resources && final->count==3 && final->error_rows>=3 &&
        !effects->outcomes_owner && effect_outcomes_empty(&effects->outcomes_work));
    const XrXirFunctionEffects *control=xr_xir_effects_function(effects,1);
    CHECK(control==&final->functions[1]);
    EffectInvocationCertificate *again=effects->invocations;EffectGraph graph={0};
    XrCompileResourceStats published=pc_stats(&context);size_t published_attempts=attempts;
    const XrXirModule *source=xr_xir_compile_artifact_module(checked);
    CHECK(effect_invocation_publication_commit(&context,effects,&again)==XR_XIR_BAD_STRUCTURE &&
        again==effects->invocations &&
        effect_invocation_publish(&context,source,effects,&graph,NULL,NULL)==XR_XIR_BAD_STRUCTURE &&
        effect_outcomes_publish(&context,source,effects)==XR_XIR_BAD_STRUCTURE &&
        pc_stats(&context).work==published.work && attempts==published_attempts &&
        effect_final_outcomes(effects)==final);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    pc_empty_golden(effects,0);CHECK(xr_xir_effects_function(effects,1)==control);
    XrXirEffects incomplete={.resources=context.resources,.count=effects->count,.invocations=effects->invocations};
    incomplete.outcomes_work.functions=final->functions;
    XrCompileResourceStats before=pc_stats(&context);size_t attempts_before=attempts;
    CHECK(!effect_final_outcomes(&incomplete) && !xr_xir_effects_function(&incomplete,1) &&
        !xr_xir_effects_suspend_witness(&incomplete,1) &&
        xr_xir_effects_go_safe(&incomplete,1)==XR_XIR_BAD_STRUCTURE &&
        xr_xir_effects_task_errors(&incomplete,1)==XR_XIR_BAD_STRUCTURE &&
        xr_xir_effects_task_creation(&incomplete,1)==XR_XIR_EFFECT_UNKNOWN &&
        !xr_xir_effects_error(&incomplete,2,(XrXirType)EE_ENUM,1));
    CHECK(pc_stats(&context).work==before.work && attempts==attempts_before);
    uint32_t count=effects->invocations->final->outcomes->count;
    effects->invocations->final->outcomes->count=count+1;
    CHECK(!effect_final_outcomes(effects) && !xr_xir_effects_function(effects,1));
    effects->invocations->final->outcomes->count=count;pc_empty_golden(effects,0);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}

static void pc_outcome_error_rows_literals(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    CHECK(final && final->count==5 && final->error_rows>5 && final->words==1 &&
        final->errors[0]==0 && final->errors[1]==1 && final->errors[2]==0 &&
        final->errors[3]==0 && final->errors[4]==0);
    XrXirEffects fresh={0};CHECK(effect_source_outcomes_prepare(&context,effects,&fresh)==XR_XIR_OK);
    XrXirCompileContext remaining=context;
    CHECK(effect_errors_seed(&effects->invocations->body->domain,&fresh,&remaining)==XR_XIR_OK);
    EffectTerms terms={.types=*effects->invocations->body->domain.types,.remaining=&remaining};
    ErrorFlow flow={.body=effect_body_owned_request(&remaining,effects->invocations->body),
        .effects=&fresh,.remaining=&remaining,.terms=&terms,.summary_count=5,.invocation=UINT32_MAX};
    CHECK(error_invocation_contexts_new(&flow,&flow.invocations)==XR_XIR_OK && flow.invocations);
    CHECK(error_invocation_summaries(&flow)==XR_XIR_OK && error_invocation_functions(&flow)==XR_XIR_OK);
    CHECK(fresh.outcomes_work.error_rows==final->error_rows &&
        effect_source_errors_match(&context,effects,&fresh)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    uint32_t poisoned=0;
    for (uint32_t n=0;n<effects->invocations->final->count;++n) {
        const EffectInvocationNode *node=&effects->invocations->final->nodes[n];
        uint32_t row=flow.invocations->nodes[n].summary;
        if (node->root!=0 || node->body!=1 || row<5) continue;
        /* These closed rotations execute known pure/Cell-read callbacks.
         * Their zero error sets are literal, independent of stored answers. */
        CHECK(row<final->error_rows && final->errors[row]==0 && fresh.outcomes_work.errors[row]==0);
        effects->invocations->final->outcomes->errors[row]=1;
        CHECK(final->errors[0]==0 && final->errors[1]==1 && final->errors[2]==0 &&
            final->errors[3]==0 && final->errors[4]==0 &&
            effect_source_errors_match(&context,effects,&fresh)==XR_XIR_BAD_STRUCTURE);
        effects->invocations->final->outcomes->errors[row]=0;++poisoned;
        CHECK(effect_source_errors_match(&context,effects,&fresh)==XR_XIR_OK);
    }
    CHECK(poisoned==5);
    printf("whole Final error rows%u public5 closed rotations%u extra-row poisoning rejected and restored\n",
        final->error_rows,poisoned);
    error_storage_free(&flow);error_invocation_contexts_free(flow.invocations);effect_terms_free(&terms);
    effect_source_outcomes_free(&fresh);xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}

static void pc_outcome_retry(const XrXirCompileContext *context,uint32_t mutation,
    XrCompileResourceLimits caps,const PcCurrentInputMeasure *normal) {
    XrCompileResourceStats before=pc_stats(context);
    uint64_t work=normal->paid.work-normal->initial.work;
    uint64_t bytes=normal->paid.allocated_bytes-normal->initial.allocated_bytes;
    CHECK(before.live_bytes==normal->initial.live_bytes);
    bool fits=work<=caps.work-before.work && bytes<=caps.allocated_bytes-before.allocated_bytes &&
        normal->paid.peak_bytes<=caps.live_bytes;
    XrXirStatus status=pc_outcome_late_operation(context,mutation,false);
    CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
    XrCompileResourceStats after=pc_stats(context);
    CHECK(after.live_bytes==before.live_bytes && after.work>=before.work &&
        after.allocated_bytes>=before.allocated_bytes);
}

static void pc_outcome_resources(void) {
    for (uint32_t mutation=0;mutation<2;++mutation) {
        PcCurrentInputMeasure normal={0};
        XrXirCompileContext context=pc_owner(pc_caps());normal.initial=pc_stats(&context);
        attempts=0;CHECK(pc_outcome_late_operation(&context,mutation,false)==XR_XIR_OK);
        normal.sites=attempts;normal.paid=pc_stats(&context);pc_release(&context,normal.initial.live_bytes);
        for (size_t ordinal=0;ordinal<normal.sites;++ordinal) {
            context=pc_owner(pc_caps());XrCompileResourceStats before=pc_stats(&context);
            XrCompileResources *identity=context.resources;
            attempts=0;injected=false;fail_at=ordinal;
            XrXirStatus status=pc_outcome_late_operation(&context,mutation,false);fail_at=SIZE_MAX;
            CHECK(injected && status==XR_XIR_OUT_OF_MEMORY && context.resources==identity &&
                pc_stats(&context).live_bytes==before.live_bytes);
            pc_outcome_retry(&context,mutation,pc_caps(),&normal);pc_release(&context,before.live_bytes);
        }
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits caps=pc_caps();
            if (!axis) caps.allocated_bytes=normal.paid.allocated_bytes-minus;
            else if (axis==1) caps.live_bytes=normal.paid.peak_bytes-minus;
            else caps.work=normal.paid.work-minus;
            context=pc_owner(caps);XrCompileResourceStats before=pc_stats(&context);
            CHECK(pc_outcome_late_operation(&context,mutation,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            pc_outcome_retry(&context,mutation,caps,&normal);pc_release(&context,before.live_bytes);
        }
        printf("whole Final late mutation%u complete sites%zu work%llu allocated%llu peak%llu all ordinals three axes same-owner physical0\n",
            mutation,normal.sites,(unsigned long long)normal.paid.work,
            (unsigned long long)normal.paid.allocated_bytes,(unsigned long long)normal.paid.peak_bytes);
    }
}
