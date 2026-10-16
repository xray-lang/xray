/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_input_seed_run_cases.h - Current Input primitive and scalar transfer probes
 */
#ifndef XIR_INPUT_SEED_RUN_CASES_H
#define XIR_INPUT_SEED_RUN_CASES_H

static void input_seed_run_roots(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,false,true);
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    XrXirRootEffects facts={true,true};
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    XrXirRootEffects saved=material->local_roots[1];
    material->local_roots[1]=(XrXirRootEffects){true,true};facts=(XrXirRootEffects){true,true};
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    material->local_roots[1]=saved;
    EffectBodyRead read=effect_body_compile_read(&context);const EffectBodyRun *borrowed=NULL;
    CHECK(effect_body_record_ref(&read,&material->functions[1],4,&borrowed)==XR_XIR_OK);
    EffectBodyRun *run=(EffectBodyRun *)borrowed;
    XrXirOp opcode=run->base.op;uint32_t mask=run->mask;
    CHECK(opcode==XR_XIR_CONST_INT && !(mask&UINT32_C(1)));
    for (uint32_t damage=0;damage<3;++damage) {
        if (!damage) run->base.op=XR_XIR_SLOT_LOAD;
        else if (damage==1) run->base.op=XR_XIR_INVALID;
        else {
            run->mask=mask|UINT32_C(1);
            run->delta[0]=(uint32_t)XR_XIR_SLOT_LOAD-(uint32_t)opcode;
        }
        facts=(XrXirRootEffects){true,true};
        CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_BAD_STRUCTURE &&
            facts.requires_root && facts.unresolved);
        run->base.op=opcode;run->mask=mask;
        CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
            !facts.requires_root && !facts.unresolved);
    }
    uint32_t unused=run->base.targets[1];run->base.targets[1]^=UINT32_C(0x12345678);
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    CHECK(effect_body_source_match(&context,material,&fixture.module)==XR_XIR_BAD_STRUCTURE);
    run->base.targets[1]=unused;
    CHECK(effect_body_source_match(&context,material,&fixture.module)==XR_XIR_OK);
    memset(&fixture,0xCE,sizeof(fixture));
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    effect_body_material_free(material);rp_owner_free(&context,baseline);rp_balanced(physical);
}

typedef struct InputScalarRead {
    const XrXirCompileContext *context;
    uint32_t calls,reject;
} InputScalarRead;
static bool input_scalar_read(const void *owner,uint64_t units) {
    InputScalarRead *read=(InputScalarRead *)owner;
    return read->calls++!=read->reject && xir_compile_work(read->context,units);
}

static void input_seed_run_scalar(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,false,true);
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    EffectBodyRequest body=effect_body_owned_request(&context,material);
    XrXirEffects effects={0};effects.count=fixture.module.function_count;
    CHECK(effect_errors_seed(&body.domain,&effects,&context)==XR_XIR_OK);
    EffectTerms terms={0};terms.remaining=&context;terms.types=*body.domain.types;
    ErrorFlow flow={0};flow.body=body;flow.effects=&effects;flow.remaining=&context;flow.terms=&terms;
    CHECK(error_function(&flow,1)==XR_XIR_OK && effects.errors[1]==UINT64_C(12));
    EffectBodyRead read=effect_body_compile_read(&context);const EffectBodyRun *borrowed=NULL;
    uint32_t i=4,value=flow.function->parameter_count+i;
    CHECK(effect_body_record_ref(&read,&material->functions[1],i,&borrowed)==XR_XIR_OK &&
        borrowed->base.op==XR_XIR_CONST_INT && borrowed->base.type==XR_XIR_I64 &&
        !(borrowed->mask&UINT32_C(3)) && flow.slots[value]==UINT32_MAX);
    XrXirStatus status=XR_XIR_OK;
    uint64_t *retained=xir_compile_copy(&context,flow.work,(size_t)flow.stride*sizeof(*retained),&status);
    CHECK(retained && status==XR_XIR_OK);
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
    ErrorBlockRead row={0};row.storage=sentinel;row.end=&sentinel;
    size_t attempts=rp_attempts;uint64_t before=rp_stats(&context).work;
    CHECK(error_block_row(&flow,0,i,NULL,borrowed,&row)==XR_XIR_OK && flow.instruction==i &&
        row.end==&sentinel && !row.decoded && conditional_compact_same(&row.storage,&sentinel) &&
        rp_attempts==attempts && rp_stats(&context).work>before &&
        !memcmp(retained,flow.work,(size_t)flow.stride*sizeof(*retained)));
    flow.slots[value]=0;
    CHECK(error_block_row(&flow,0,i,NULL,borrowed,&row)==XR_XIR_BAD_STRUCTURE &&
        row.end==&sentinel && !memcmp(retained,flow.work,(size_t)flow.stride*sizeof(*retained)));
    flow.slots[value]=UINT32_MAX;
    EffectBodyRun *run=(EffectBodyRun *)borrowed;uint32_t mask=run->mask;
    XrXirType type=run->base.type;
    for (uint32_t variant=0;variant<3;++variant) {
        if (!variant) run->base.type=XR_XIR_BOOL;
        else { run->mask=mask|(variant==1?UINT32_C(1):UINT32_C(2));run->delta[variant-1]=1; }
        row=(ErrorBlockRead){0};row.storage=sentinel;row.end=&sentinel;
        CHECK(error_block_row(&flow,0,i,NULL,borrowed,&row)==XR_XIR_OK && row.decoded &&
            row.end==&row.storage && row.storage.op==XR_XIR_CONST_INT);
        run->base.type=type;run->mask=mask;
    }
    for (uint32_t cut=0;cut<2;++cut) {
        InputScalarRead observation={&context,0,cut};flow.body.read=(EffectBodyRead){&observation,input_scalar_read};
        row=(ErrorBlockRead){0};row.storage=sentinel;row.end=&sentinel;
        CHECK(error_block_row(&flow,0,i,NULL,borrowed,&row)==XR_XIR_BUDGET &&
            observation.calls==cut+1 && row.end==&sentinel && !row.decoded &&
            !memcmp(retained,flow.work,(size_t)flow.stride*sizeof(*retained)));
        flow.body.read=read;
        CHECK(error_block_row(&flow,0,i,NULL,borrowed,&row)==XR_XIR_OK && row.end==&sentinel);
    }
    row=(ErrorBlockRead){0};row.storage=sentinel;row.end=&sentinel;
    uint32_t last=flow.function->instruction_count-1;
    CHECK(effect_body_record_ref(&read,&material->functions[1],last,&borrowed)==XR_XIR_OK);
    CHECK(error_block_row(&flow,0,last,NULL,borrowed,&row)==XR_XIR_OK && row.decoded &&
        row.end==&row.storage && row.storage.op==XR_XIR_THROW);
    CHECK(effect_body_source_match(&context,material,&fixture.module)==XR_XIR_OK);
    xr_compile_resources_free(retained);error_storage_free(&flow);effect_terms_free(&terms);
    xr_compile_resources_free(effects.errors);xr_compile_resources_free(effects.atoms);
    effect_body_material_free(material);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void input_seed_run_literals(void) {
    input_seed_run_roots();input_seed_run_scalar();
    puts("current Input ROOT seeds and integer Error transfer literals passed");
}
#endif // XIR_INPUT_SEED_RUN_CASES_H
