/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_class_cell_cases.h - Exact Class subjects at real Cell terminals
 */
#ifndef XIR_INVOCATION_CLASS_CELL_CASES_H
#define XIR_INVOCATION_CLASS_CELL_CASES_H

typedef struct InvocationClassCellFixture {
    InvocationClassRowsFixture source;
    XrXirTypeNode nodes[3];XrXirType cell;
    XrXirFunction functions[6];XrXirFunctionIdentity identities[6];
    XrXirInstruction entry[15],read[2];uint32_t operands[8];
    XrXirBlock blocks[4],read_block;
    uint32_t place,project;
} InvocationClassCellFixture;

/* The original seven independent Class relations gain an actual projection
 * and direct ref consumer. Their values and mixed-source expectations stay
 * literal; no query result constructs a permission or selects the subject. */
static void invocation_class_cell_fixture(InvocationClassCellFixture *f,uint32_t mode) {
    CHECK(mode<7);memset(f,0,sizeof(*f));invocation_class_rows_fixture(&f->source,mode);
    InvocationClassRowsFixture *s=&f->source;f->cell=(XrXirType)258;
    s->field.flags=XR_XIR_FIELD_MUTABLE;
    memcpy(f->nodes,s->nodes,sizeof(s->nodes));
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    s->types.nodes=f->nodes;s->types.count=3;
    memcpy(f->functions,s->functions,sizeof(s->functions));
    memcpy(f->identities,s->identities,sizeof(s->identities));
    const XrXirFunction *entry=&s->functions[3];uint32_t count=entry->instruction_count;
    CHECK(count && count+4<=15 && entry->operand_count+1<=8 && entry->block_count<=4);
    memcpy(f->entry,entry->instructions,(size_t)count*sizeof(*f->entry));
    memcpy(f->operands,entry->operands,(size_t)entry->operand_count*sizeof(*f->operands));
    memcpy(f->blocks,entry->blocks,(size_t)entry->block_count*sizeof(*f->blocks));
    uint32_t returned=f->entry[count-1].args[0];CHECK(f->entry[count-1].op==XR_XIR_RETURN);
    f->place=count-1;f->project=count+1;
    f->entry[count-1]=(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=(XrXirType)256,.args={returned}};
    f->entry[count]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={count-1}};
    f->entry[count+1]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={count}};
    f->entry[count+2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,
        .args={entry->operand_count,1},.immediate=5};
    f->entry[count+3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={returned}};
    f->operands[entry->operand_count]=count+1;f->blocks[entry->block_count-1].count+=4;
    f->functions[3].instructions=f->entry;f->functions[3].instruction_count=count+4;
    f->functions[3].operands=f->operands;f->functions[3].operand_count=entry->operand_count+1;
    f->functions[3].blocks=f->blocks;
    f->read[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->read[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->read_block=(XrXirBlock){0,2,0,0};
    f->functions[5]=(XrXirFunction){"read",4,&f->cell,1,XR_XIR_I64,&f->read_block,1,f->read,2,NULL,0};
    s->declarations.functions=f->identities;s->module.functions=f->functions;s->module.function_count=6;
}

static void conditional_class_cell_controls(const XrXirCompileContext *context,
    XrXirEffects *effects,uint32_t place,uint32_t project,uint32_t mode) {
    EffectInvocationCertificate *certificate=effects->invocations;
    EffectInvocationRead replay={0};
    XrXirStatus opening=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
    CHECK(opening==XR_XIR_OK);
    replay.work=context;replay.effects=effects;replay.declared=certificate->declared;
    EffectInvocationFlow flow={.read=&replay,.node=replay.final->roots[3]};
    EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
    CHECK(effect_invocation_origins_read(&flow,&body)==XR_XIR_OK);
    XrXirCellOriginView legacy={0},actual={0};
    CHECK(xr_xir_compile_cell_origin_view(context,effects->cells,3,project,&legacy)==XR_XIR_OK &&
        (legacy.intrinsic_mask&XR_XIR_CELL_ACCESS_UNKNOWN));
    CHECK(effect_invocation_class_cell_view(&flow,project,&actual)==XR_XIR_OK &&
        actual.intrinsic_mask==(mode<3?0u:XR_XIR_CELL_ACCESS_UNKNOWN));
    XrXirCellOriginView sentinel={.intrinsic_mask=9};uint64_t before=rp_stats(context).work;
    CHECK(effect_invocation_class_cell_view(&flow,UINT32_MAX,&sentinel)==XR_XIR_BAD_STRUCTURE &&
        sentinel.intrinsic_mask==9 && rp_stats(context).work==before);
    XrXirInstruction storage={0};const XrXirInstruction *op=NULL;
    CHECK(effect_invocation_flow_instruction(&flow,place,&storage,&op)==XR_XIR_OK);
    uint32_t at=0;CHECK(op->op==XR_XIR_OBJECT_PLACE &&
        effect_invocation_class_find(context,&replay.final->nodes[flow.node],op->args[0],&at)==XR_XIR_OK);
    EffectInvocationClassFact *fact=&replay.final->nodes[flow.node].classes[at],saved=*fact;
    fact->mask=0;fact->rank=0;actual=(XrXirCellOriginView){0};
    CHECK(effect_invocation_class_cell_view(&flow,project,&actual)==XR_XIR_UNRESOLVED &&
        !actual.intrinsic_mask && !actual.dependencies && !actual.parameter_count);*fact=saved;
    fact->mask=1;fact->rank=0;actual=(XrXirCellOriginView){0};
    CHECK(effect_invocation_class_cell_view(&flow,project,&actual)==XR_XIR_BAD_STRUCTURE);*fact=saved;
    CHECK(effect_invocation_class_terminals_match(&flow)==XR_XIR_OK);
    EffectInvocationNode *node=&replay.final->nodes[flow.node];
    uint32_t local_mask=node->local_mask,intrinsic_mask=node->intrinsic_mask;
    if (mode<3) {
        EffectInvocationCause *local=&effect_invocation_local_causes(node)[0];
        EffectInvocationCause saved_local=*local,saved_global=node->causes[0];
        node->local_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;node->intrinsic_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
        *local=(EffectInvocationCause){{XR_XIR_ROOT_CAUSE_CELL_ACCESS,project,UINT32_MAX,project-1,0},
            UINT32_MAX,UINT32_MAX,0};node->causes[0]=*local;
        CHECK(effect_invocation_class_terminals_match(&flow)==XR_XIR_BAD_STRUCTURE);
        *local=saved_local;node->causes[0]=saved_global;
    } else {
        node->local_mask&=~XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        node->intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        CHECK(effect_invocation_class_terminals_match(&flow)==XR_XIR_BAD_STRUCTURE);
    }
    node->local_mask=local_mask;node->intrinsic_mask=intrinsic_mask;
    CHECK(effect_invocation_class_terminals_match(&flow)==XR_XIR_OK);
    xr_compile_resources_free(flow.memory);effect_invocation_read_close(&replay);
}

static XrXirStatus conditional_class_cell_case(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);
    uint32_t place=fixture.place,project=fixture.project;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_effects_analyze(checked,&effects); }
    if (status==XR_XIR_OK && oracle) {
        XrXirModule *exposed=(XrXirModule *)xr_xir_compile_artifact_module(checked);
        XrXirInstruction *ops=(XrXirInstruction *)exposed->functions[3].instructions;
        XrXirType old=ops[place].type;ops[place].type=XR_XIR_I64;
        CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_BAD_TYPE);ops[place].type=old;
        CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_OK);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) { phase=3;status=conditional_class_rows_replay(context,effects); }
    if (status==XR_XIR_OK && oracle) {
        CHECK(!effect_roots_read(effects)->root[3].requires_root && effect_roots_read(effects)->root[3].unresolved==(mode>=3));
        conditional_class_cell_controls(context,effects,place,project,mode);
        CHECK(conditional_class_rows_replay(context,effects)==XR_XIR_OK);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_CLASS_CELL mode=%u phase=%u status=%u f=%u b=%u i=%u\n",
        mode,phase,(uint32_t)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

static void conditional_class_cell_literals(void) {
    for (uint32_t mode=0;mode<7;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_class_cell_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_class_cell_resources(void) {
    for (uint32_t mode=0;mode<7;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_class_cell_case(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context);
                fprintf(stderr,"INVOCATION_CLASS_CELL_CENSUS mode=%u sites=%zu allocated=%llu live=%llu peak=%llu work=%llu\n",
                    mode,sites,(unsigned long long)single.allocated_bytes,(unsigned long long)single.live_bytes,
                    (unsigned long long)single.peak_bytes,(unsigned long long)single.work);
            } else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
                ConditionalRetryTrial retry={.which=90+mode,.ordinal=pass-1,.sites=sites,.resources=identity,
                    .initial=entry,.single=single,.retained=retained,.first=status};
                conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={single.allocated_bytes,single.peak_bytes,single.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
            CHECK(measured[axis]);RootParameterMark physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
            uint64_t value=measured[axis]-(pass?0:1);
            if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
            XrXirCompileContext context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            XrXirStatus status=conditional_class_cell_case(&context,mode,false);
            CHECK(status==(pass?XR_XIR_OK:XR_XIR_BUDGET));
            ConditionalRetryTrial retry={.which=90+mode,.ordinal=SIZE_MAX,.sites=sites,.resources=identity,
                .initial=entry,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        CHECK(conditional_retry_counts[90+mode]>=6);
    }
}
#endif // XIR_INVOCATION_CLASS_CELL_CASES_H
