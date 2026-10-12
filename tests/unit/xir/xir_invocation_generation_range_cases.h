/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_generation_range_cases.h - Fresh control dependency ranges
 */
#ifndef XIR_INVOCATION_GENERATION_RANGE_CASES_H
#define XIR_INVOCATION_GENERATION_RANGE_CASES_H

typedef struct GenerationRangeTrial {
    InvocationDeferredFixture fixture;
    XrXirInstruction entry[8],worker[2];
    XrXirTypeNode nodes[3];
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirDefaultBinding binding;XrXirDefaultTable defaults;
    XrXirArtifact *checked;XrXirEffects effects;EffectGraph graph;
} GenerationRangeTrial;

/* The original declaration-complete producer is reused. GO adds a real pure
 * worker; DEFAULT adds an ordinary I64 owner and two genuine I64 producers. */
static void generation_range_fixture(GenerationRangeTrial *trial,uint32_t mode) {
    memset(trial,0,sizeof(*trial));invocation_deferred_fixture(&trial->fixture,0);
    InvocationDeferredFixture *fixture=&trial->fixture;
    conditional_generation_alias_fixture(fixture,trial->entry,0);
    if (!mode) return;
    memcpy(trial->functions,fixture->functions,sizeof(fixture->functions));
    memcpy(trial->nodes,fixture->nodes,sizeof(fixture->nodes));
    trial->worker[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    trial->worker[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    trial->functions[4]=(XrXirFunction){.name="alternate",.name_length=9,.result=XR_XIR_I64,
        .blocks=&fixture->two,.block_count=1,.instructions=trial->worker,.instruction_count=2};
    fixture->module.functions=trial->functions;fixture->module.function_count=5;
    fixture->declarations.functions=trial->identities;
    if (mode==1) {
        trial->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};
        fixture->types.nodes=trial->nodes;fixture->types.count=3;
        trial->entry[6]=trial->entry[5];
        trial->entry[5]=(XrXirInstruction){.op=XR_XIR_GO,.type=(XrXirType)258,.immediate=4};
        fixture->entry_block.count=trial->functions[0].instruction_count=7;return;
    }
    trial->entry[0]=(XrXirInstruction){.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={1,0}};
    trial->entry[1]=trial->entry[0];
    trial->entry[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    fixture->entry_block.count=trial->functions[0].instruction_count=3;
    trial->functions[0].operands=NULL;trial->functions[0].operand_count=0;
    fixture->parameters[0]=XR_XIR_I64;
    trial->functions[1]=(XrXirFunction){.name="owner",.name_length=5,.result=XR_XIR_I64,
        .parameters=fixture->parameters,.parameter_count=1,.blocks=&fixture->init_block,
        .block_count=1,.instructions=fixture->init,.instruction_count=1};
    trial->functions[2]=trial->functions[4];trial->functions[2].name="default";
    trial->functions[2].name_length=7;
    trial->binding=(XrXirDefaultBinding){XR_XIR_DEFAULT_PARAMETER,1,0,mode==2?2u:4u};
    trial->defaults=(XrXirDefaultTable){&trial->binding,1};fixture->module.defaults=&trial->defaults;
}

static XrXirStatus generation_range_prepare(const XrXirCompileContext *context,
    GenerationRangeTrial *trial) {
    XrXirStatus status=xir_fixture_check(context,&trial->fixture.module,&trial->checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(trial->checked,NULL);
    const XrXirModule *source=trial->checked?xr_xir_compile_artifact_module(trial->checked):NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&trial->effects);
    if (status==XR_XIR_OK) status=effect_parameters_derive(source,&trial->effects,&trial->graph,context);
    return status;
}

/* Graph arrays are private on every result. The existing generation owner
 * survives each attempt; neither its quotas nor cumulative fees are reset. */
static void generation_range_build(const XrXirCompileContext *context,
    GenerationRangeTrial *trial,XrXirStatus expected,bool write_failure,uint32_t target) {
    const XrXirModule *source=xr_xir_compile_artifact_module(trial->checked);
    EffectBodyMaterial *material=trial->effects.generation;
    CHECK(material==trial->graph.material && !trial->graph.physical_owned);
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;
    XrXirStatus status=effect_graph_build_material(source,&trial->effects,&trial->graph,
        (XrXirCompileContext *)context);
    CHECK(status==expected);
    if (write_failure) CHECK(trial->graph.heads && trial->graph.queue && trial->graph.queued);
    if (status==XR_XIR_OK && target!=UINT32_MAX) {
        CHECK(trial->graph.edge_count==2 && trial->graph.heads[target]==1);
        CHECK(trial->graph.edges[0].caller==0 && trial->graph.edges[0].instruction==0 &&
            trial->graph.edges[0].next==UINT32_MAX && !trial->graph.edges[0].cleanup);
        CHECK(trial->graph.edges[1].caller==0 && trial->graph.edges[1].instruction==1 &&
            trial->graph.edges[1].next==0 && !trial->graph.edges[1].cleanup);
        CHECK(trial->graph.heads[target==2?4:2]==UINT32_MAX);
    }
    effect_graph_free(&trial->graph);
    trial->graph=(EffectGraph){.material=material,.physical=material->physical};
    CHECK(rp_stats(context).live_bytes==held_live);rp_balanced(held);
}

/* Both independent walks start at zero. A malformed last record must leave
 * the failed step's numeric position and output unchanged. Full source and
 * physical correspondence remain separate terminal obligations. */
static void generation_range_eof(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyFunction *function=&material->functions[0];
    CHECK(function->stream_words && function->stream_words<function->stream_capacity);
    uint32_t words=function->stream_words;EffectBodyRead read=effect_body_compile_read(context);
    for (uint32_t pass=0;pass<2;++pass) for (uint32_t extra=0;extra<2;++extra) {
        function->stream_words=extra?words+1:words-1;
        EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};XrXirStatus status=XR_XIR_OK;
        while (status==XR_XIR_OK && cursor.run<function->run_count) {
            EffectBodyRecordCursor before=cursor;
            EffectBodyRunRef out={.first=UINT32_MAX,.count=UINT32_MAX},sentinel=out;
            status=effect_body_range_ref(&read,function,&cursor,&out);
            if (status!=XR_XIR_OK) CHECK(status==XR_XIR_BAD_STRUCTURE &&
                !memcmp(&cursor,&before,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
        }
        CHECK(status==XR_XIR_BAD_STRUCTURE);
        CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_BAD_STRUCTURE);
        function->stream_words=words;
    }
}

static void generation_range_physical(const XrXirCompileContext *context,GenerationRangeTrial *trial) {
    EffectBodyMaterial *material=trial->effects.generation;
    const XrXirModule *source=xr_xir_compile_artifact_module(trial->checked);
    EffectPhysicalFunction *physical=&material->physical->functions[0];
    CHECK(physical->count>2);
    EffectPhysicalRecord first=physical->records[0],last=physical->records[physical->count-1];
    physical->records[0]=last;physical->records[physical->count-1]=first;
    CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_BAD_STRUCTURE);
    generation_range_build(context,trial,XR_XIR_BAD_STRUCTURE,false,UINT32_MAX);
    physical->records[0]=first;physical->records[physical->count-1]=last;
    uint32_t reference=UINT32_MAX;
    for (uint32_t r=0;r<physical->count;++r)
        if (physical->records[r].mask&EFFECT_PHYSICAL_REFERENCE) reference=r;
    CHECK(reference!=UINT32_MAX);
    uint32_t mask=physical->records[reference].mask;
    physical->records[reference].mask|=EFFECT_PHYSICAL_DIRECT;
    CHECK(effect_body_physical_match(context,material->physical,material)==XR_XIR_BAD_STRUCTURE);
    generation_range_build(context,trial,XR_XIR_BAD_STRUCTURE,true,UINT32_MAX);
    physical->records[reference].mask=mask;
    EffectPhysicalFunction *formals=&material->physical->functions[1];
    CHECK(formals->count && formals->records[0].value<formals->parameters);
    mask=formals->records[0].mask;formals->records[0].mask|=EFFECT_PHYSICAL_DIRECT;
    generation_range_build(context,trial,XR_XIR_BAD_STRUCTURE,false,UINT32_MAX);
    formals->records[0].mask=mask;
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK &&
        effect_body_physical_match(context,material->physical,material)==XR_XIR_OK);
    generation_range_build(context,trial,XR_XIR_OK,false,UINT32_MAX);
}

static uint32_t generation_range_type_reads,generation_range_consume_reads;

static XrXirStatus generation_range_counted_type(const EffectBodyRequest *request,
    uint32_t function,uint32_t value,XrXirType *output) {
    ++generation_range_type_reads;return effect_body_owned_type(request,function,value,output);
}

static bool generation_range_consume(const void *owner,uint64_t units) {
    ++generation_range_consume_reads;return effect_body_compile_consume(owner,units);
}

/* This calls the pass directly, after a genuine complete origin replay. A
 * different immutable owner on the same ledger is not its equation's body. */
static void generation_range_origins_refused(EffectInvocationFlow *flow) {
    const XrXirCompileContext *context=flow->owner->work;
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    uint32_t count=flow->owner->count,pending=flow->owner->pending;
    void *memory=flow->memory;bool changed=flow->changed;
    CHECK(effect_invocation_origins_pass(flow)==XR_XIR_BAD_STRUCTURE &&
        flow->owner->count==count && flow->owner->pending==pending && flow->memory==memory &&
        flow->changed==changed && rp_attempts==attempts && rp_stats(context).work==before.work &&
        rp_stats(context).live_bytes==before.live_bytes);
    bool dispatched=false;
    CHECK(effect_invocation_dispatch(flow,&dispatched)==XR_XIR_BAD_STRUCTURE && !dispatched &&
        flow->owner->count==count && flow->owner->pending==pending && flow->memory==memory &&
        rp_attempts==attempts && rp_stats(context).work==before.work &&
        rp_stats(context).live_bytes==before.live_bytes);
}

/* Each delegated callback changes only request dispatch. The reference pass
 * uses the counted instruction reader; all other mixed passes must preserve
 * the original scalar pass's real fees and settled origin rows. */
static void generation_range_mixed_origins(EffectInvocationFlow *flow) {
    EffectBodyRequest original=flow->body;uint32_t transfers=0;
    for (uint32_t r=0;r<flow->physical->count;++r)
        transfers+=!!(flow->physical->records[r].mask&EFFECT_PHYSICAL_TRANSFER);
    CHECK(transfers && !flow->changed && !flow->owner->producer_enabled);
    uint64_t scalar_work=0;conditional_generation_instruction_refused=false;
    conditional_generation_shape_refused=conditional_generation_dimensions_refused=false;
    for (uint32_t mixed=0;mixed<5;++mixed) {
        flow->body=original;conditional_generation_instruction_reads=0;
        generation_range_type_reads=generation_range_consume_reads=0;
        if (!mixed) flow->body.instruction=conditional_generation_counted_instruction;
        else if (mixed==1) flow->body.head=conditional_generation_counted_head;
        else if (mixed==2) flow->body.type=generation_range_counted_type;
        else if (mixed==3) flow->body.shape=conditional_generation_counted_shape;
        else flow->body.read.consume=generation_range_consume;
        if (mixed==2) {
            XrXirType actual=XR_XIR_UNIT;
            CHECK(flow->body.type!=effect_body_owned_type &&
                effect_body_request_type(&flow->body,flow->owner->nodes[flow->node].body,0,&actual)==XR_XIR_OK &&
                actual==XR_XIR_I64 && generation_range_type_reads==1);
        } else if (mixed==4) CHECK(flow->body.read.consume!=effect_body_compile_consume);
        XrCompileResourceStats before=rp_stats(flow->owner->work);size_t attempts=rp_attempts;
        CHECK(effect_invocation_origins_pass(flow)==XR_XIR_OK && !flow->changed &&
            rp_attempts==attempts && rp_stats(flow->owner->work).live_bytes==before.live_bytes);
        uint64_t cost=rp_stats(flow->owner->work).work-before.work;
        if (!mixed) {
            CHECK(conditional_generation_instruction_reads==transfers && cost);scalar_work=cost;
        } else CHECK(cost==scalar_work);
        if (mixed==4) CHECK(generation_range_consume_reads);
    }
    flow->body=original;
    CHECK(effect_invocation_origins_pass(flow)==XR_XIR_OK && !flow->changed);
}

/* Replaying a settled equation preserves every actual key, edge and ROOT
 * fact. Delegated readers exercise the original complete scalar dispatcher;
 * the canonical request must enter the same decoded instruction handler. */
static void generation_range_dispatch_same(EffectInvocationFlow *flow) {
    EffectBodyRequest original=flow->body;EffectInvocationOwner *owner=flow->owner;
    uint32_t count=owner->count,edges=owner->edge_count,mask=owner->nodes[flow->node].intrinsic_mask;
    bool deferred=owner->nodes[flow->node].deferred;
    uint32_t selected=0;
    for (uint32_t r=0;r<flow->physical->count;++r)
        selected+=!!(flow->physical->records[r].mask&EFFECT_PHYSICAL_INVOCATION);
    conditional_generation_instruction_refused=false;
    conditional_generation_shape_refused=conditional_generation_dimensions_refused=false;
    for (uint32_t precision=0;precision<2;++precision) {
        flow->precision=precision!=0;uint64_t scalar_work=0;
        for (uint32_t mixed=0;mixed<7;++mixed) {
            flow->body=original;conditional_generation_instruction_reads=0;
            generation_range_type_reads=generation_range_consume_reads=0;
            if (!mixed) flow->body.instruction=conditional_generation_counted_instruction;
            else if (mixed==1) flow->body.head=conditional_generation_counted_head;
            else if (mixed==2) flow->body.type=generation_range_counted_type;
            else if (mixed==3) flow->body.shape=conditional_generation_counted_shape;
            else if (mixed==4) flow->body.read.consume=generation_range_consume;
            else if (mixed==6) flow->body=effect_body_raw_request(owner->work,owner->module);
            if (mixed==2) {
                XrXirType actual=XR_XIR_UNIT;
                CHECK(flow->body.type!=effect_body_owned_type &&
                    effect_body_request_type(&flow->body,owner->nodes[flow->node].body,0,&actual)==XR_XIR_OK &&
                    actual==XR_XIR_I64 && generation_range_type_reads==1);
            } else if (mixed==4) CHECK(flow->body.read.consume!=effect_body_compile_consume);
            XrCompileResourceStats before=rp_stats(owner->work);bool changed=false;
            CHECK(effect_invocation_dispatch(flow,&changed)==XR_XIR_OK && !changed &&
                owner->count==count && owner->edge_count==edges && !owner->pending &&
                owner->nodes[flow->node].intrinsic_mask==mask &&
                owner->nodes[flow->node].deferred==deferred &&
                rp_stats(owner->work).live_bytes==before.live_bytes);
            uint64_t cost=rp_stats(owner->work).work-before.work;
            if (!mixed) {
                CHECK(conditional_generation_instruction_reads==selected && cost);scalar_work=cost;
            } else if (mixed<5) CHECK(cost==scalar_work);
            if (mixed==4) CHECK(generation_range_consume_reads);
            if (mixed==5) printf("generation dispatch body%u precision%u selected%u scalar%llu canonical%llu\n",
                owner->nodes[flow->node].body,precision,selected,(unsigned long long)scalar_work,
                (unsigned long long)cost);
        }
    }
    flow->body=original;flow->precision=false;
}

/* Missing EOF and an actual out-of-domain CALL target fail in the request,
 * before any new key or edge is published. Restoring the physical words does
 * not refund their fees or manufacture a reusable permission fact. */
static void generation_range_dispatch_faults(EffectInvocationFlow *flow) {
    EffectInvocationOwner *owner=flow->owner;EffectBodyMaterial *material=owner->material;
    uint32_t f=owner->nodes[flow->node].body;EffectBodyFunction *body=&material->functions[f];
    CHECK(flow->physical->count>1 && body->run_count>1 && body->stream_words);
    uint32_t words=body->stream_words,count=owner->count,edges=owner->edge_count;
    for (uint32_t precision=0;precision<2;++precision) {
        flow->precision=precision!=0;bool changed=false;XrCompileResourceStats before=rp_stats(owner->work);
        --body->stream_words;
        CHECK(effect_invocation_dispatch(flow,&changed)==XR_XIR_BAD_STRUCTURE && !changed &&
            owner->count==count && owner->edge_count==edges && !owner->pending &&
            rp_stats(owner->work).live_bytes==before.live_bytes);
        body->stream_words=words;
    }
    flow->precision=false;
    uint32_t call=UINT32_MAX;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        CHECK(effect_body_request_instruction(&flow->body,f,i,&storage,&op)==XR_XIR_OK);
        if (op->op==XR_XIR_CALL) { call=i;break; }
    }
    uint32_t selected=0,first=UINT32_MAX;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        if (!selected) first=record.value-body->parameter_count;
        ++selected;
    }
    CHECK(selected==2 && first==2 && call==4);EffectBodyRunRef ref;
    /* The genuine CALL and RETURN are the final two-row affine run. The
     * first selected FnRef is earlier; only the second selected CALL changes. */
    CHECK(effect_body_record_ref(&flow->body.read,body,call,&ref)==XR_XIR_OK &&
        ref.first==call && ref.count==2 && ref.base[0]==XR_XIR_CALL &&
        (ref.mask&(UINT32_C(1)<<6)) && !(ref.mask&(UINT32_C(1)<<7)));
    uint32_t *base=(uint32_t *)ref.base,low=base[6],high=base[7];
    base[6]=effect_body_count(material);base[7]=0;
    for (uint32_t precision=0;precision<2;++precision) {
        flow->precision=precision!=0;flow->instruction=UINT32_MAX;
        XrCompileResourceStats before=rp_stats(owner->work);bool changed=false;
        CHECK(effect_invocation_dispatch(flow,&changed)==XR_XIR_BAD_STRUCTURE && !changed &&
            flow->instruction==call && owner->count==count && owner->edge_count==edges &&
            !owner->pending && rp_stats(owner->work).live_bytes==before.live_bytes);
    }
    base[6]=low;base[7]=high;flow->precision=false;bool changed=false;
    CHECK(effect_invocation_dispatch(flow,&changed)==XR_XIR_OK && !changed);
}

/* A genuine one-row initializer retains the complete scalar path and its
 * original cost. Its declaration is neither empty-body permission nor NONE. */
static void generation_range_dispatch_singleton(EffectInvocationOwner *owner) {
    EffectInvocationFlow flow={.owner=owner,.node=owner->roots[3],.instruction=UINT32_MAX};
    CHECK(effect_invocation_origins(&flow)==XR_XIR_OK && !flow.changed &&
        (flow.physical->count<=1 || owner->material->functions[3].run_count<=1));
    EffectBodyRequest original=flow.body;
    for (uint32_t precision=0;precision<2;++precision) {
        flow.precision=precision!=0;bool changed=false;XrCompileResourceStats before=rp_stats(owner->work);
        CHECK(effect_invocation_dispatch(&flow,&changed)==XR_XIR_OK && !changed);
        uint64_t canonical=rp_stats(owner->work).work-before.work;
        flow.body.instruction=conditional_generation_counted_instruction;
        before=rp_stats(owner->work);
        CHECK(effect_invocation_dispatch(&flow,&changed)==XR_XIR_OK && !changed &&
            rp_stats(owner->work).work-before.work==canonical &&
            rp_stats(owner->work).live_bytes==before.live_bytes);
        flow.body=original;
    }
    xr_compile_resources_free(flow.memory);
}

static void generation_range_origin_bindings(const XrXirCompileContext *context,
    GenerationRangeTrial *trial) {
    const XrXirModule *source=xr_xir_compile_artifact_module(trial->checked);
    EffectBodyMaterial *material=trial->effects.generation,*other=NULL;
    CHECK(effect_graph_build_material(source,&trial->effects,&trial->graph,
        (XrXirCompileContext *)context)==XR_XIR_OK);
    EffectInvocationOwner *owner=NULL;
    CHECK(effect_invocations_create(context,source,&trial->effects,&trial->graph,
        material->declared,&owner)==XR_XIR_OK && effect_invocations_solve(owner)==XR_XIR_OK);
    CHECK(!owner->producer_enabled && !owner->pending && owner->material==material &&
        owner->physical==material->physical);
    EffectInvocationFlow flow={.owner=owner,.node=owner->roots[0],.instruction=UINT32_MAX};
    CHECK(effect_invocation_origins(&flow)==XR_XIR_OK && !flow.changed);
    uint32_t function=owner->nodes[flow.node].body;
    CHECK(function==0 && flow.physical==&material->physical->functions[function] &&
        effect_invocation_origins_pass(&flow)==XR_XIR_OK && !flow.changed);
    CHECK(effect_body_material_create(context,source,&other)==XR_XIR_OK && other!=material &&
        other->resources==material->resources && effect_body_count(other)==effect_body_count(material));
    for (uint32_t f=0;f<effect_body_count(material);++f)
        CHECK(other->functions[f].parameter_count==material->functions[f].parameter_count &&
            other->functions[f].instruction_count==material->functions[f].instruction_count);
    EffectBodyRequest original=flow.body;flow.body=effect_body_owned_request(context,other);
    generation_range_origins_refused(&flow);flow.body=original;
    EffectPhysicalDomain *physical=owner->physical;owner->physical=other->physical;
    generation_range_origins_refused(&flow);owner->physical=physical;
    const EffectPhysicalFunction *current=flow.physical;
    flow.physical=&material->physical->functions[1];generation_range_origins_refused(&flow);
    flow.physical=current;
    uint32_t count=material->physical->count;++material->physical->count;
    generation_range_origins_refused(&flow);material->physical->count=count;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_entry=rp_stats(&foreign);
    size_t foreign_attempts=rp_attempts;
    flow.body.read.owner=&foreign;generation_range_origins_refused(&flow);flow.body=original;
    CHECK(rp_stats(&foreign).live_bytes==foreign_entry.live_bytes &&
        rp_stats(&foreign).allocated_bytes==foreign_entry.allocated_bytes &&
        rp_stats(&foreign).work==foreign_entry.work && rp_attempts==foreign_attempts);
    rp_owner_free(&foreign,foreign_entry.live_bytes);
    generation_range_mixed_origins(&flow);
    generation_range_dispatch_same(&flow);generation_range_dispatch_faults(&flow);
    generation_range_dispatch_singleton(owner);
    xr_compile_resources_free(flow.memory);effect_invocation_free(owner);
    effect_body_material_free(other);effect_graph_free(&trial->graph);
    trial->graph=(EffectGraph){.material=material,.physical=material->physical};
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK &&
        effect_body_physical_match(context,material->physical,material)==XR_XIR_OK);
}

static void generation_range_dispatch_other(const XrXirCompileContext *context,
    GenerationRangeTrial *trial) {
    const XrXirModule *source=xr_xir_compile_artifact_module(trial->checked);
    EffectBodyMaterial *material=trial->effects.generation;EffectInvocationOwner *owner=NULL;
    CHECK(effect_graph_build_material(source,&trial->effects,&trial->graph,
        (XrXirCompileContext *)context)==XR_XIR_OK &&
        effect_invocations_create(context,source,&trial->effects,&trial->graph,
            material->declared,&owner)==XR_XIR_OK && effect_invocations_solve(owner)==XR_XIR_OK);
    EffectInvocationFlow flow={.owner=owner,.node=owner->roots[0],.instruction=UINT32_MAX};
    CHECK(effect_invocation_origins(&flow)==XR_XIR_OK && !flow.changed &&
        flow.physical->count>1 && material->functions[0].run_count>1);
    generation_range_dispatch_same(&flow);generation_range_dispatch_singleton(owner);
    xr_compile_resources_free(flow.memory);effect_invocation_free(owner);effect_graph_free(&trial->graph);
    trial->graph=(EffectGraph){.material=material,.physical=material->physical};
}

static void generation_range_controls(void) {
    for (uint32_t mode=0;mode<4;++mode) {
        RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;GenerationRangeTrial trial;
        generation_range_fixture(&trial,mode);CHECK(generation_range_prepare(&context,&trial)==XR_XIR_OK);
        EffectBodyMaterial *material=trial.effects.generation;
        generation_range_build(&context,&trial,XR_XIR_OK,false,mode<2?UINT32_MAX:mode==2?2u:4u);
        if (!mode) {
            generation_range_eof(&context,material);
            uint32_t words=material->functions[0].stream_words;--material->functions[0].stream_words;
            generation_range_build(&context,&trial,XR_XIR_BAD_STRUCTURE,false,UINT32_MAX);
            material->functions[0].stream_words=words;generation_range_physical(&context,&trial);
            generation_range_origin_bindings(&context,&trial);
        } else if (mode==1) {
            EffectPhysicalFunction *physical=&material->physical->functions[0];uint32_t go=UINT32_MAX;
            for (uint32_t r=0;r<physical->count;++r)
                if (physical->records[r].mask&EFFECT_PHYSICAL_GO) go=r;
            CHECK(go!=UINT32_MAX && trial.effects.task_creation[0]==XR_XIR_EFFECT_MAY);
            uint32_t mask=physical->records[go].mask;physical->records[go].mask|=EFFECT_PHYSICAL_DIRECT;
            generation_range_build(&context,&trial,XR_XIR_BAD_STRUCTURE,true,UINT32_MAX);
            physical->records[go].mask=mask;
        } else {
            CHECK(material->domain.defaults && material->domain.defaults->count==1);
            XrXirDefaultBinding *binding=(XrXirDefaultBinding *)material->domain.defaults->records;
            uint32_t function=binding->function;binding->function=5;
            generation_range_build(&context,&trial,XR_XIR_BAD_STRUCTURE,true,UINT32_MAX);
            binding->function=function;
            generation_range_build(&context,&trial,XR_XIR_OK,false,mode==2?2u:4u);
        }
        if (mode) generation_range_dispatch_other(&context,&trial);
        const XrXirModule *source=xr_xir_compile_artifact_module(trial.checked);
        CHECK(effect_body_source_match(&context,material,source)==XR_XIR_OK &&
            effect_body_physical_match(&context,material->physical,material)==XR_XIR_OK);
        effect_graph_free(&trial.graph);invocation_core_storage_free(&trial.effects);
        xr_xir_compile_artifact_free(trial.checked);rp_owner_free(&context,baseline);rp_balanced(held);
    }
}
#endif // XIR_INVOCATION_GENERATION_RANGE_CASES_H
