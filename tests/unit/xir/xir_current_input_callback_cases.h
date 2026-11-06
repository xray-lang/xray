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

static void pc_current_input_mutate(void *memory) {
    size_t bytes=pc_final_physical_size(memory);
    CHECK(memory && bytes && pc_current_input_row && !pc_current_input_calls);
    rp_allocation_observer=NULL;++pc_current_input_calls;
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
    CHECK(mutation<=10 && !rp_allocation_observer && !pc_current_input_row);
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectGraph graph={0};
    XrXirModule source={0};XrXirFunction functions[3];XrXirInstruction rows[EE_MAX_OPS];
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=effect_infer_construct(context,(XirEffectInput){xr_xir_compile_artifact_module(checked),xr_xir_compile_artifact_construction(checked),NULL},NULL,&effects);
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
        memcpy(facts,effect_roots_read(effects)->root,sizeof(facts));memcpy(contracts,effect_roots_read(effects)->contracts,sizeof(contracts));
        memcpy(witnesses,effect_roots_read(effects)->root_witnesses,sizeof(witnesses)/2);
        memcpy(witnesses+3,effect_roots_read(effects)->unresolved_witnesses,sizeof(witnesses)/2);
        XrCompileResourceStats before=rp_stats(context);
        pc_current_input_row=&rows[4];pc_current_input_mutation=mutation;pc_current_input_calls=0;
        if (mutation) rp_allocation_observer=pc_current_input_mutate;
        status=effect_invocation_publish(context,&source,effects,&graph,NULL,NULL);
        rp_allocation_observer=NULL;pc_current_input_row=NULL;rows[4]=saved;
        if (oracle) fprintf(stderr,"current raw publication mutation%u callbacks%u status%u retained%u\n",
            mutation,pc_current_input_calls,status,effects->invocations==old.invocations);
        if (status==XR_XIR_BAD_STRUCTURE && mutation && pc_current_input_calls==1) {
            CHECK(!memcmp(effects,&old,sizeof(old)) && !memcmp(effect_roots_read(effects)->root,facts,sizeof(facts)) &&
                !memcmp(effect_roots_read(effects)->contracts,contracts,sizeof(contracts)) &&
                !memcmp(effect_roots_read(effects)->root_witnesses,witnesses,sizeof(witnesses)/2) &&
                !memcmp(effect_roots_read(effects)->unresolved_witnesses,witnesses+3,sizeof(witnesses)/2));
            CHECK(rp_stats(context).allocated_bytes>before.allocated_bytes);
            status=XR_XIR_OK;
        } else if (status==XR_XIR_OK) {
            CHECK(!mutation && !pc_current_input_calls && effects->invocations!=old.invocations);
        } else CHECK(status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_MEMORY_LIMIT || status==XR_XIR_BUDGET);
        effect_graph_free(&graph);graph=(EffectGraph){0};
        if (status==XR_XIR_OK) {
            XrXirCompileContext copy=*context;status=effect_graph_build(&source,effects,&graph,&copy);
        }
        if (status==XR_XIR_OK) status=effect_invocation_publish(context,&source,effects,&graph,NULL,NULL);
        if (status==XR_XIR_OK) CHECK(effect_roots_read(effects)->root[0].requires_root &&
            rp_stats(context).allocated_bytes>before.allocated_bytes);
        /* Building a new graph initializes provisional control rows. Complete
         * every original effect derivation again before the sole outcome move. */
        XrXirCompileContext final=*context;
        if(status==XR_XIR_OK) status=effect_propagate(effects,&graph);
        if(status==XR_XIR_OK) status=effect_go_safe(&source,effects,&graph,&final);
        if(status==XR_XIR_OK) status=effect_witnesses(&source,effects,&graph,&final);
        if(status==XR_XIR_OK) status=local_errors_owned(&final,graph.material,NULL);
        if(status==XR_XIR_OK) status=local_error_outcomes(&final,graph.material,effects->outcomes_work.functions,false);
        if(status==XR_XIR_OK) {
            const XirEffectEntryRootView *sealed=effects->outcomes_work.entry_roots;
            size_t allocations=rp_attempts;XrCompileResourceStats retained=rp_stats(context);
            CHECK(sealed && effect_entry_roots_seal(&source,effects,&final)==XR_XIR_BAD_STRUCTURE &&
                effects->outcomes_work.entry_roots==sealed && rp_attempts==allocations &&
                conditional_memory_unchanged(context,retained));
        }

    }
    effect_graph_free(&graph);
    if (status==XR_XIR_OK) status=effect_outcomes_publish(context,&source,effects);
    if (status==XR_XIR_OK) pc_empty_golden(effects,0);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    CHECK(!rp_allocation_observer && !pc_current_input_row);return status;
}

static void pc_current_input_callbacks(void) {
    for (uint32_t mutation=0;mutation<=10;++mutation) {
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
        CHECK(pc_current_input_operation(&context,mutation,true)==XR_XIR_OK);rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
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
    XrCompileResourceStats before=rp_stats(context);
    bool fits=normal->paid.peak_bytes<=caps.memory_limit;
    XrXirStatus status=pc_current_input_operation(context,mutation,false);
    XrCompileResourceStats after=rp_stats(context);
    CHECK(status==(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT) && after.held_bytes==before.held_bytes &&
        after.allocated_bytes>=before.allocated_bytes &&
        after.allocation_count>=before.allocation_count);
}

static PcCurrentInputMeasure pc_current_input_measure(uint32_t mutation,XrCompileResourceLimits caps,
    XrXirStatus expected,const PcCurrentInputMeasure *normal) {
    XrXirCompileContext context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();PcCurrentInputMeasure measure={0};
    measure.initial=rp_stats(&context);rp_attempts=0;
    CHECK(pc_current_input_operation(&context,mutation,false)==expected);
    measure.paid=rp_stats(&context);measure.sites=rp_attempts;
    CHECK(measure.paid.held_bytes==measure.initial.held_bytes);
    if (normal) pc_current_input_retry(&context,mutation,caps,normal);
    rp_owner_free(&context,measure.initial.held_bytes);CHECK(!rp_live && !rp_live_bytes);return measure;
}

static void pc_current_input_resources(void) {
    size_t total=0;
    for (uint32_t mutation=0;mutation<=10;++mutation) {
        XrCompileResourceLimits caps=rp_caps();
        PcCurrentInputMeasure normal=pc_current_input_measure(mutation,caps,XR_XIR_OK,NULL);
        CHECK(normal.sites && normal.sites<4096 &&
            normal.sites==normal.paid.allocation_count-normal.initial.allocation_count);
        for (size_t site=0;site<normal.sites;++site) {
            XrXirCompileContext context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
            rp_attempts=0;rp_injected=false;rp_fail_at=site;
            XrXirStatus status=pc_current_input_operation(&context,mutation,false);rp_fail_at=SIZE_MAX;
            CHECK(rp_injected && rp_attempts>site && status==XR_XIR_OUT_OF_MEMORY &&
                rp_stats(&context).held_bytes==baseline);
            pc_current_input_retry(&context,mutation,caps,&normal);rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
        }
        XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,normal.paid.peak_bytes,true};
        PcCurrentInputMeasure same=pc_current_input_measure(mutation,exact,XR_XIR_OK,&normal);
        CHECK(same.sites==normal.sites && same.paid.allocated_bytes==normal.paid.allocated_bytes &&
            same.paid.peak_bytes==normal.paid.peak_bytes);
        XrCompileResourceLimits less=exact;CHECK(less.memory_limit>1);--less.memory_limit;
        (void)pc_current_input_measure(mutation,less,XR_XIR_MEMORY_LIMIT,&normal);
        total+=normal.sites;
        printf("current input callback mutation%u complete sites%zu allocated%llu peak%llu "
            "all ordinals held exact/short same-owner physical0\n",mutation,normal.sites,
            (unsigned long long)normal.paid.allocated_bytes,(unsigned long long)normal.paid.peak_bytes);
    }
    printf("current input callback all eleven modes allocation sites%zu PASS\n",total);
}
#endif // XIR_CURRENT_INPUT_CALLBACK_CASES_H
