/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_terminal_callback_cases.h - Last current Input after receiving callbacks
 */
static void pc_final_terms_retired(const EffectTerms *terms,const char *name) {
    fprintf(stderr,"published terms %s remaining%u stack%u capacity%u match%u ledger%u\n",name,
        terms->remaining!=NULL,terms->stack!=NULL,terms->stack_capacity,
        terms->match.memory!=NULL,terms->match.resources!=NULL);
    CHECK(!terms->remaining && !terms->stack && !terms->cache && !terms->stack_capacity &&
        !terms->stack_claimed && !terms->match.memory && !terms->match.resources);
}
static void pc_final_body_terms_retired(const EffectBodyMaterial *body,const char *name) {
    uint32_t depth=0;
    for (;body;body=body->backbone) {
        CHECK(depth++<3);pc_final_terms_retired(&body->terms,name);
    }
}
static void pc_final_all_terms_retired(const XrXirEffects *effects) {
    CHECK(effect_final_outcomes(effects) && effects->invocations);
    pc_final_terms_retired(&effects->invocations->terms,"outer-certificate");
    pc_final_body_terms_retired(effects->invocations->body,"outer-input");
    const EffectContextOwner *owner=effect_contexts_read(effects);
    CHECK(owner && owner->ordinary && owner->dense && owner->forest && owner->forest->invocations);
    pc_final_terms_retired(&owner->ordinary->terms,"ordinary");
    pc_final_terms_retired(&owner->dense->terms,"dense");
    pc_final_terms_retired(&owner->forest->terms,"forest");
    pc_final_terms_retired(&owner->forest->invocations->terms,"forest-certificate");
    pc_final_body_terms_retired(owner->forest->invocations->body,"forest-input");
    CHECK(effect_ordinary_work_empty(owner->ordinary) && effect_ordinary_work_empty(owner->dense) &&
        owner->ordinary->resources==effects->resources && owner->dense->resources==effects->resources);
}

static void pc_final_retired_terms_rejections(const XrXirCompileContext *context,
    XirFixtureEffectInput source,XrXirEffects *effects) {
    EffectTerms *terms=&effects->invocations->final->outcomes->contexts->ordinary->terms;
    EffectTerms saved=*terms;
    for (uint32_t mode=0;mode<7;++mode) {
        switch (mode) {
        case 0:terms->stack=(EffectTermFrame *)(uintptr_t)1;break;
        case 1:terms->cache=(XrXirType *)(uintptr_t)1;break;
        case 2:terms->stack_capacity=1;break;
        case 3:terms->stack_claimed=true;break;
        case 4:terms->match.memory=(struct TypeMatchMemory *)(uintptr_t)1;break;
        case 5:terms->match.resources=(XrCompileResources *)(uintptr_t)1;break;
        default:terms->remaining=(const XrXirCompileContext *)(uintptr_t)1;break;
        }
        XrXirStatus status=xir_fixture_effects_terminal(context,source,effects);
        *terms=saved;
        CHECK(status==XR_XIR_BAD_STRUCTURE);
    }
    CHECK(xir_fixture_effects_terminal(context,source,effects)==XR_XIR_OK);
    fprintf(stderr,"published constructor scratch resurrection: seven fields rejected and sameowner restored\n");
}

static XrXirInstruction *pc_source_terminal_row;
static size_t pc_source_terminal_skip,pc_source_terminal_observed,pc_source_terminal_free_count;
static uint32_t pc_source_terminal_mutation,pc_source_terminal_calls;

static void pc_source_terminal_trigger(void *memory,size_t bytes) {
    CHECK(memory && bytes && pc_source_terminal_row);
    if (pc_source_terminal_observed++<pc_source_terminal_skip) return;
    CHECK(!pc_source_terminal_calls);++pc_source_terminal_calls;pc_allocate_hook=NULL;pc_free_hook=NULL;
    CHECK(pc_source_terminal_row->op==XR_XIR_CONST_INT && pc_source_terminal_row->type==XR_XIR_I64);
    if (pc_source_terminal_mutation) pc_source_terminal_row->type=XR_XIR_BOOL;
}

static void pc_source_terminal_count_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);++pc_source_terminal_free_count;
}

static XrXirStatus pc_source_terminal_operation(const XrXirCompileContext *context,
    uint32_t mutation,bool oracle) {
    InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    pc_final_all_terms_retired(effects);
    const EffectFinalOutcomes *published=effect_final_outcomes(effects);
    CHECK(published && published->contexts && !effects->outcomes_work.contexts &&
        effect_contexts_read(effects)==published->contexts);
    const EffectContextOwner *original=published->contexts;
    size_t calls=attempts;uint64_t observed_work=pc_stats(context).work;
    CHECK(xir_fixture_effects_refresh(context,
        (XirFixtureEffectInput){xr_xir_compile_artifact_module(checked),
        xr_xir_compile_artifact_construction(checked)},effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(attempts==calls && pc_stats(context).work==observed_work);
    CHECK(effect_contexts_read(effects)==original);
    const XrXirModule *input=xr_xir_compile_artifact_module(checked);
    if (!mutation) pc_final_retired_terms_rejections(context,
        (XirFixtureEffectInput){input,xr_xir_compile_artifact_construction(checked)},effects);
    XrXirModule current=*input;XrXirFunction functions[5];XrXirInstruction rows[2];
    CHECK(input->function_count==5 && input->functions[2].instruction_count==2);
    memcpy(functions,input->functions,sizeof(functions));memcpy(rows,functions[2].instructions,sizeof(rows));
    current.functions=functions;functions[2].instructions=rows;
    CHECK(!pc_free_hook);pc_source_terminal_free_count=0;pc_free_hook=pc_source_terminal_count_free;
    size_t before=attempts;
    CHECK(effect_source_relations(context,effects->invocations,effects)==XR_XIR_OK);
    CHECK(effect_source_contexts(context,&current,effects)==XR_XIR_OK);
    CHECK(effect_source_publication_match(context,&current,effects)==XR_XIR_OK);
    size_t prefix_allocations=attempts-before,prefix_frees=pc_source_terminal_free_count;pc_free_hook=NULL;
    size_t prefix=mutation==2?prefix_frees:prefix_allocations;
    CHECK(!pc_allocate_hook && !pc_source_terminal_row && mutation<3);
    CHECK(prefix);pc_source_terminal_skip=prefix-1;pc_source_terminal_observed=0;pc_source_terminal_calls=0;
    pc_source_terminal_mutation=mutation;pc_source_terminal_row=&rows[0];
    if (mutation==2) pc_free_hook=pc_source_terminal_trigger;else pc_allocate_hook=pc_source_terminal_trigger;
    XrXirStatus status=xir_fixture_effects_terminal(context,
        (XirFixtureEffectInput){&current,xr_xir_compile_artifact_construction(checked)},effects);
    pc_allocate_hook=NULL;pc_free_hook=NULL;pc_source_terminal_row=NULL;
    CHECK(pc_source_terminal_calls==1 && pc_source_terminal_observed==prefix);
    rows[0].type=XR_XIR_I64;
    CHECK(xir_fixture_effects_terminal(context,
        (XirFixtureEffectInput){&current,xr_xir_compile_artifact_construction(checked)},effects)==XR_XIR_OK);
    if (oracle) fprintf(stderr,"late Source receiving mutation%u prefix%zu callback%u status%u restored0\n",
        mutation,prefix,pc_source_terminal_calls,status);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
    xr_xir_compile_effects_free(effects);return status;
}

static void pc_source_terminal_literal(uint32_t mutation) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirStatus status=pc_source_terminal_operation(&context,mutation,true);
    pc_release(&context,baseline);
    CHECK(status==(mutation?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
}

#include "xir_ordinary_retirement_cases.h"

#include "xir_role_reach_cases.h"
#include "xir_dense_retirement_cases.h"

#include "xir_role_source_final_cases.h"

static void pc_source_terminal_literals(void) {
    pc_role_source_final_literals();
    pc_role_reach_literals();
    pc_ordinary_retirement_literals();
    pc_dense_retirement_literals();
    for (uint32_t mutation=0;mutation<3;++mutation) pc_source_terminal_literal(mutation);
}
