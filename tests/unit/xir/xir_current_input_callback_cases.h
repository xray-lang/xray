/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_current_input_callback_cases.h - Exact current instruction publication
 */
#ifndef XIR_CURRENT_INPUT_CALLBACK_CASES_H
#define XIR_CURRENT_INPUT_CALLBACK_CASES_H
static XrXirInstruction *pc_current_input_row;
static uint32_t pc_current_input_mutation,pc_current_input_calls;

static void pc_current_input_mutate(void *memory,size_t bytes) {
    CHECK(memory && bytes && pc_current_input_row && !pc_current_input_calls);
    pc_allocate_hook=NULL;++pc_current_input_calls;
    XrXirInstruction *row=pc_current_input_row;
    switch (pc_current_input_mutation) {
        case 1: row->op=XR_XIR_RETURN;break;
        case 2: row->type=XR_XIR_BOOL;break;
        case 3: row->args[0]^=1;break;
        case 4: row->args[1]^=1;break;
        case 5: row->targets[0]^=1;break;
        case 6: row->targets[1]^=1;break;
        case 7: row->immediate^=1;break;
        case 8: row->immediate^=INT64_C(1)<<32;break;
        case 9: row->type_arguments[0]^=1;break;
        case 10: row->type_arguments[1]^=1;break;
        default: CHECK(false);
    }
}

/* The raw receiving candidate has independently mutable rows. Its genuine
 * Checked producer and genuine unpublished constructor precede the real allocator callback.
 * Only the expected publication rejection is normalized; all preparation,
 * allocator and budget failures retain their actual status. */
static XrXirStatus pc_current_input_operation(const XrXirCompileContext *context,
    uint32_t mutation,bool oracle) {
    CHECK(mutation<=10 && !pc_allocate_hook && !pc_current_input_row);
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectGraph graph={0};
    XrXirModule source={0};XrXirFunction functions[3];XrXirInstruction rows[EE_MAX_OPS];
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=effect_infer_construct(context,xr_xir_compile_artifact_module(checked),NULL,&effects);
    if (status==XR_XIR_OK) {
        const XrXirModule *original=xr_xir_compile_artifact_module(checked);
        CHECK(original->function_count==3 && original->functions[1].instruction_count<=EE_MAX_OPS);
        source=*original;memcpy(functions,original->functions,sizeof(functions));
        memcpy(rows,functions[1].instructions,(size_t)functions[1].instruction_count*sizeof(*rows));
        source.functions=functions;functions[1].instructions=rows;
        CHECK(rows[4].type==XR_XIR_I64);
        XrXirCompileContext copy=*context;status=effect_graph_build(&source,effects,&graph,&copy);
    }
    if (status==XR_XIR_OK) status=effect_body_source_match(context,graph.material,&source);
    if (status==XR_XIR_OK) {
        XrXirInstruction saved=rows[4];XrXirEffects old=*effects;
        XrXirRootEffects facts[3];XrXirRootEffectWitness witnesses[6];
        XrXirFunctionEffectContract contracts[3];
        memcpy(facts,effects->root,sizeof(facts));memcpy(contracts,effects->contracts,sizeof(contracts));
        memcpy(witnesses,effects->root_witnesses,sizeof(witnesses)/2);
        memcpy(witnesses+3,effects->unresolved_witnesses,sizeof(witnesses)/2);
        XrCompileResourceStats before=pc_stats(context);
        pc_current_input_row=&rows[4];pc_current_input_mutation=mutation;pc_current_input_calls=0;
        if (mutation) pc_allocate_hook=pc_current_input_mutate;
        status=effect_invocation_publish(context,&source,effects,&graph,NULL,NULL);
        pc_allocate_hook=NULL;pc_current_input_row=NULL;rows[4]=saved;
        if (oracle) fprintf(stderr,"current raw publication mutation%u callbacks%u status%u retained%u\n",
            mutation,pc_current_input_calls,status,effects->invocations==old.invocations);
        if (status==XR_XIR_BAD_STRUCTURE && mutation && pc_current_input_calls==1) {
            CHECK(!memcmp(effects,&old,sizeof(old)) && !memcmp(effects->root,facts,sizeof(facts)) &&
                !memcmp(effects->contracts,contracts,sizeof(contracts)) &&
                !memcmp(effects->root_witnesses,witnesses,sizeof(witnesses)/2) &&
                !memcmp(effects->unresolved_witnesses,witnesses+3,sizeof(witnesses)/2));
            CHECK(pc_stats(context).work>before.work &&
                pc_stats(context).allocated_bytes>before.allocated_bytes);
            status=XR_XIR_OK;
        } else if (status==XR_XIR_OK) {
            CHECK(!mutation && !pc_current_input_calls && effects->invocations!=old.invocations);
        } else CHECK(status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET);
        effect_graph_free(&graph);graph=(EffectGraph){0};
        if (status==XR_XIR_OK) {
            XrXirCompileContext copy=*context;status=effect_graph_build(&source,effects,&graph,&copy);
        }
        if (status==XR_XIR_OK) status=effect_invocation_publish(context,&source,effects,&graph,NULL,NULL);
        if (status==XR_XIR_OK) CHECK(effects->root[0].requires_root &&
            pc_stats(context).work>before.work && pc_stats(context).allocated_bytes>before.allocated_bytes);
    }
    effect_graph_free(&graph);
    if (status==XR_XIR_OK) status=effect_outcomes_publish(context,&source,effects);
    if (status==XR_XIR_OK) pc_empty_golden(effects,0);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    CHECK(!pc_allocate_hook && !pc_current_input_row);return status;
}

static void pc_current_input_callbacks(void) {
    for (uint32_t mutation=0;mutation<=10;++mutation) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        CHECK(pc_current_input_operation(&context,mutation,true)==XR_XIR_OK);pc_release(&context,baseline);
    }
}

typedef struct PcCurrentInputMeasure {
    XrCompileResourceStats initial,paid;
    size_t sites;
} PcCurrentInputMeasure;

/* Retry retains the failed attempt's ledger. Whether the complete operation
 * still fits follows from its actual census and the remaining original caps. */
static void pc_current_input_retry(const XrXirCompileContext *context,uint32_t mutation,
    XrCompileResourceLimits caps,const PcCurrentInputMeasure *normal) {
    XrCompileResourceStats before=pc_stats(context);
    uint64_t work=normal->paid.work-normal->initial.work;
    uint64_t allocated=normal->paid.allocated_bytes-normal->initial.allocated_bytes;
    bool fits=before.work<=caps.work && work<=caps.work-before.work &&
        before.allocated_bytes<=caps.allocated_bytes && allocated<=caps.allocated_bytes-before.allocated_bytes &&
        normal->paid.peak_bytes<=caps.live_bytes;
    XrXirStatus status=pc_current_input_operation(context,mutation,false);
    XrCompileResourceStats after=pc_stats(context);
    CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET) && after.live_bytes==before.live_bytes &&
        after.work>=before.work && after.allocated_bytes>=before.allocated_bytes);
    if (fits) CHECK(after.work-before.work==work && after.allocated_bytes-before.allocated_bytes==allocated &&
        after.allocation_count-before.allocation_count==normal->paid.allocation_count-normal->initial.allocation_count);
}

static PcCurrentInputMeasure pc_current_input_measure(uint32_t mutation,XrCompileResourceLimits caps,
    XrXirStatus expected,const PcCurrentInputMeasure *normal) {
    XrXirCompileContext context=pc_owner(caps);PcCurrentInputMeasure measure={0};
    measure.initial=pc_stats(&context);attempts=0;
    CHECK(pc_current_input_operation(&context,mutation,false)==expected);
    measure.paid=pc_stats(&context);measure.sites=attempts;
    CHECK(measure.paid.live_bytes==measure.initial.live_bytes);
    if (normal) pc_current_input_retry(&context,mutation,caps,normal);
    pc_release(&context,measure.initial.live_bytes);return measure;
}

static void pc_current_input_resources(void) {
    size_t total=0;
    for (uint32_t mutation=0;mutation<=10;++mutation) {
        XrCompileResourceLimits caps=pc_caps();
        PcCurrentInputMeasure normal=pc_current_input_measure(mutation,caps,XR_XIR_OK,NULL);
        CHECK(normal.sites && normal.sites<4096 &&
            normal.sites==normal.paid.allocation_count-normal.initial.allocation_count);
        for (size_t site=0;site<normal.sites;++site) {
            XrXirCompileContext context=pc_owner(caps);uint64_t baseline=pc_stats(&context).live_bytes;
            attempts=0;injected=false;fail_at=site;
            XrXirStatus status=pc_current_input_operation(&context,mutation,false);fail_at=SIZE_MAX;
            CHECK(injected && attempts>site && status==XR_XIR_OUT_OF_MEMORY &&
                pc_stats(&context).live_bytes==baseline);
            pc_current_input_retry(&context,mutation,caps,&normal);pc_release(&context,baseline);
        }
        XrCompileResourceLimits exact={normal.paid.allocated_bytes,normal.paid.peak_bytes,normal.paid.work};
        PcCurrentInputMeasure same=pc_current_input_measure(mutation,exact,XR_XIR_OK,&normal);
        CHECK(same.sites==normal.sites && same.paid.allocated_bytes==normal.paid.allocated_bytes &&
            same.paid.peak_bytes==normal.paid.peak_bytes && same.paid.work==normal.paid.work);
        XrCompileResourceLimits less=exact;--less.allocated_bytes;
        (void)pc_current_input_measure(mutation,less,XR_XIR_BUDGET,&normal);
        less=exact;--less.live_bytes;(void)pc_current_input_measure(mutation,less,XR_XIR_BUDGET,&normal);
        less=exact;--less.work;(void)pc_current_input_measure(mutation,less,XR_XIR_BUDGET,&normal);
        total+=normal.sites;
        printf("current input callback mutation%u complete sites%zu allocated%llu peak%llu work%llu "
            "all ordinals three axes same-owner physical0\n",mutation,normal.sites,
            (unsigned long long)normal.paid.allocated_bytes,(unsigned long long)normal.paid.peak_bytes,
            (unsigned long long)normal.paid.work);
    }
    printf("current input callback all eleven modes allocation sites%zu PASS\n",total);
}
#endif // XIR_CURRENT_INPUT_CALLBACK_CASES_H
