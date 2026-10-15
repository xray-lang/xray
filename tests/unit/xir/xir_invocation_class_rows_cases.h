/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_class_rows_cases.h - Complete Class sources on genuine call inputs
 */
#ifndef XIR_INVOCATION_CLASS_ROWS_CASES_H
#define XIR_INVOCATION_CLASS_ROWS_CASES_H

typedef struct InvocationClassRowsFixture {
    XrXirNominalField field;
    XrXirNominalDeclaration declaration;
    XrXirNominalTable nominals;
    XrXirType field_type,constructor_parameter,forward_parameter;
    XrXirTypeNode nodes[2];
    XrXirTypes types;
    XrXirInstruction init[1],constructor[2],forward[2],entry[11],main[3];
    uint32_t constructor_operands[1],forward_operands[1],entry_operands[7];
    XrXirBlock init_block,constructor_block,forward_block,entry_blocks[4],main_block;
    XrXirFunction functions[5];
    XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} InvocationClassRowsFixture;

/* All variants are literal independent graphs. The mixed variants change a
 * real producer or write, rather than manufacturing an input permission. */
static void invocation_class_rows_fixture(InvocationClassRowsFixture *f,uint32_t mode) {
    CHECK(mode<7);memset(f,0,sizeof(*f));
    const XrXirType object=(XrXirType)256,cell=(XrXirType)257;
    f->field=(XrXirNominalField){{"value",5},XR_XIR_I64,XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_MUTABLE};
    f->declaration=(XrXirNominalDeclaration){.module={"root",4},.name={"RowObject",9},.exported=1,
        .fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals=(XrXirNominalTable){&f->declaration,1,NULL};f->field_type=XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&f->field_type,1}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=object};
    f->types=(XrXirTypes){f->nodes,2,&f->nominals,NULL};
    f->constructor_parameter=XR_XIR_I64;f->forward_parameter=object;
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->constructor[0]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=object,.args={0,1}};
    f->constructor[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->forward[0]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={0,1},.immediate=1};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=2};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};
    f->entry_operands[0]=0;f->entry_operands[1]=1;
    uint32_t entry_count=4,operand_count=2,block_count=1,forward_count=1;
    if (mode==1 || mode==4) {
        f->entry[2]=mode==1 ?
            (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7} :
            (XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
        f->entry[3]=mode==1 ?
            (XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=1} :
            (XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={2}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={4},.targets={1,2}};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
        f->entry[7]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
        f->entry[8]=(XrXirInstruction){.op=XR_XIR_PHI,.type=object,.args={2,4}};
        f->entry[9]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={6,1},.immediate=2};
        f->entry[10]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={9}};
        f->entry_operands[1]=2;f->entry_operands[2]=1;f->entry_operands[3]=1;
        f->entry_operands[4]=2;f->entry_operands[5]=3;f->entry_operands[6]=8;
        if (mode==4) {
            /* The second constructor call is absent; the scalar operand is
             * removed so the independent full checker owns every slice. */
            f->entry[8].args[0]=1;f->entry[9].args[0]=5;
            f->entry_operands[1]=1;f->entry_operands[2]=1;
            f->entry_operands[3]=2;f->entry_operands[4]=3;f->entry_operands[5]=8;
        }
        f->entry_blocks[0]=(XrXirBlock){0,6,0,0};f->entry_blocks[1]=(XrXirBlock){6,1,0,0};
        f->entry_blocks[2]=(XrXirBlock){7,1,0,0};f->entry_blocks[3]=(XrXirBlock){8,3,0,0};
        entry_count=11;operand_count=mode==4?6:7;block_count=4;
    } else if (mode==2 || mode==6) {
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=object,.args={1}};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=1};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={2,4}};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=object,.args={2}};
        f->entry[7]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={2,1},.immediate=2};
        f->entry[8]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={7}};
        f->entry_operands[1]=3;f->entry_operands[2]=6;entry_count=9;operand_count=3;
        if (mode==6) {
            f->entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
            f->entry[4]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={3}};
            f->entry[7].args[0]=1;f->entry_operands[1]=6;operand_count=2;
        }
    } else if (mode==3) {
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={2}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=2};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};
        f->entry_operands[1]=3;entry_count=6;
    } else if (mode==5) {
        f->forward[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={0,1},.immediate=2};
        f->forward[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};forward_count=2;
    }
    f->init_block=(XrXirBlock){0,1,0,0};f->constructor_block=(XrXirBlock){0,2,0,0};
    f->forward_block=(XrXirBlock){0,forward_count,0,0};
    if (block_count==1) f->entry_blocks[0]=(XrXirBlock){0,entry_count,0,0};
    f->functions[0]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->init_block,1,f->init,1,NULL,0};
    f->functions[1]=(XrXirFunction){"new",3,&f->constructor_parameter,1,object,
        &f->constructor_block,1,f->constructor,2,f->constructor_operands,1};
    f->functions[2]=(XrXirFunction){"forward",7,&f->forward_parameter,1,object,
        &f->forward_block,1,f->forward,forward_count,mode==5?f->forward_operands:NULL,mode==5?1u:0u};
    f->functions[3]=(XrXirFunction){"entry",5,NULL,0,object,f->entry_blocks,block_count,
        f->entry,entry_count,f->entry_operands,operand_count};
    /* The Class graph is an ordinary function; a Program entry returns I64. */
    f->main[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.immediate=3};
    f->main[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->main[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->main_block=(XrXirBlock){0,3,0,0};
    f->functions[4]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,
        &f->main_block,1,f->main,3,NULL,0};
    f->identities[1]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
    f->source=(XrXirSourceModule){"root",4,NULL,0,0, 0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=4};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,5,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}

/* This copy borrows already-owned sealed material solely for fresh proof
 * replay. It never mutates an equation or derives runtime authority. */
static XrXirStatus conditional_class_rows_replay(const XrXirCompileContext *context,
    XrXirEffects *effects) {
    EffectInvocationCertificate *certificate=effects->invocations;
    EffectInvocationWorkspace replay={0};
    XrXirStatus opening=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
    if (opening!=XR_XIR_OK) return opening;
    if (!effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    replay.work=context;replay.effects=effects;
    replay.declared=certificate->declared;
    EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
    XrXirStatus status=effect_invocation_class_verify_read(&replay,&body);
    effect_invocation_read_close(&replay);return status;
}

static void conditional_class_rows_rejections(const XrXirCompileContext *context,
    XrXirEffects *effects,uint32_t node,uint32_t value,uint32_t expected) {
    EffectInvocationNode *entry=&effects->invocations->final->nodes[node];uint32_t at=0;
    CHECK(effect_invocation_class_find(context,entry,value,&at)==XR_XIR_OK);
    EffectInvocationClassFact saved=entry->classes[at];
    entry->classes[at].mask=expected==1?5:1;entry->classes[at].rank=expected==1?0:1;
    CHECK(conditional_class_rows_replay(context,effects)==XR_XIR_BAD_STRUCTURE);entry->classes[at]=saved;
    entry->classes[at].rank=expected==1?0:1;
    CHECK(conditional_class_rows_replay(context,effects)==XR_XIR_BAD_STRUCTURE);entry->classes[at]=saved;
    uint32_t count=entry->class_count;entry->class_count=count-1;
    CHECK(conditional_class_rows_replay(context,effects)==XR_XIR_BAD_STRUCTURE);entry->class_count=count;
    CHECK(conditional_class_rows_replay(context,effects)==XR_XIR_OK);
    uint32_t output=7;uint64_t work=rp_stats(context).work;size_t attempts=rp_attempts;
    CHECK(xir_effects_class_value(context,effects,node,3,value,&output)==XR_XIR_BAD_STRUCTURE &&
        output==7 && rp_stats(context).work==work && rp_attempts==attempts);
    output=0;CHECK(xir_effects_class_value(context,effects,node,2,value,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xir_effects_class_value(context,effects,node,3,0,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xir_effects_class_value(context,effects,node,3,UINT32_MAX,&output)==XR_XIR_BAD_STRUCTURE && !output);
    RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&foreign).live_bytes;
    CHECK(xir_effects_class_value(&foreign,effects,node,3,value,&output)==XR_XIR_BAD_STRUCTURE && !output);
    rp_owner_free(&foreign,baseline);rp_balanced(physical);
}

static XrXirStatus conditional_class_rows_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    InvocationClassRowsFixture fixture;invocation_class_rows_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_effects_analyze(checked,&effects); }
    if (status==XR_XIR_OK && oracle) {
        XrXirModule *exposed=(XrXirModule *)xr_xir_compile_artifact_module(checked);
        XrXirInstruction *ops=(XrXirInstruction *)exposed->functions[3].instructions;
        XrXirType original=ops[1].type;ops[1].type=XR_XIR_I64;
        CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_BAD_TYPE);ops[1].type=original;
        CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_OK);
        if (mode==6) {
            /* A local place itself is not a CALL value. Its read retains
             * the initializer and every later write in the Class proof. */
            uint32_t *operands=(uint32_t *)exposed->functions[3].operands;
            CHECK(operands[1]==6);operands[1]=2;
            CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_BAD_VALUE);
            operands[1]=6;
            CHECK(xr_xir_compile_artifact_verify(checked,&diagnostic)==XR_XIR_OK);
        }
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) { phase=3;status=conditional_class_rows_replay(context,effects); }
    if (status==XR_XIR_OK && oracle) {
        EffectInvocationCertificate *certificate=effects->invocations;
        EffectInvocationFinal *owner=certificate->final;
        const uint32_t values[7]={2,9,7,4,9,2,7},masks[7]={1,1,1,5,5,5,5};
        uint32_t selected=owner->roots[3],output=0;
        CHECK(owner->nodes[selected].class_count);
        CHECK(xir_effects_class_value(context,effects,selected,3,values[mode],&output)==XR_XIR_OK && output==masks[mode]);
        if (mode==6) {
            output=0;CHECK(xir_effects_class_value(context,effects,selected,3,2,&output)==XR_XIR_OK && output==5);
        }
        output=0;CHECK(xir_effects_class_value(context,effects,owner->roots[2],2,0,&output)==XR_XIR_OK && output==5);
        CHECK(owner->nodes[owner->roots[1]].class_return_mask==1 && owner->nodes[owner->roots[1]].class_return_rank);
        CHECK(owner->nodes[selected].class_return_mask==masks[mode]);
        conditional_class_rows_rejections(context,effects,selected,values[mode],masks[mode]);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,"INVOCATION_CLASS_ROWS mode=%u phase=%u status=%u f=%u b=%u i=%u\n",
        mode,phase,(uint32_t)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

static void conditional_class_rows_literals(void) {
    for (uint32_t mode=0;mode<7;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_class_rows_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_class_rows_resources(void) {
    for (uint32_t mode=0;mode<7;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_class_rows_case(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context);
                fprintf(stderr,"INVOCATION_CLASS_ROWS_CENSUS mode=%u sites=%zu allocated=%llu live=%llu peak=%llu work=%llu\n",
                    mode,sites,(unsigned long long)single.allocated_bytes,(unsigned long long)single.live_bytes,
                    (unsigned long long)single.peak_bytes,(unsigned long long)single.work);
            } else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
                ConditionalRetryTrial retry={.which=75+mode,.ordinal=pass-1,.sites=sites,.resources=identity,
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
            XrXirStatus status=conditional_class_rows_case(&context,mode,false);
            CHECK(status==(pass?XR_XIR_OK:XR_XIR_BUDGET));
            ConditionalRetryTrial retry={.which=75+mode,.ordinal=SIZE_MAX,.sites=sites,.resources=identity,
                .initial=entry,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_CLASS_ROWS_CASES_H
