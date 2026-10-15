/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_terminal_callback_cases.h - Last current Input after receiving callbacks
 */
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
    const XrXirModule *input=xr_xir_compile_artifact_module(checked);
    XrXirModule current=*input;XrXirFunction functions[5];XrXirInstruction rows[2];
    CHECK(input->function_count==5 && input->functions[2].instruction_count==2);
    memcpy(functions,input->functions,sizeof(functions));memcpy(rows,functions[2].instructions,sizeof(rows));
    current.functions=functions;functions[2].instructions=rows;
    CHECK(!pc_free_hook);pc_source_terminal_free_count=0;pc_free_hook=pc_source_terminal_count_free;
    size_t before=attempts;
    CHECK(effect_source_certificate(context,effects->invocations,effects)==XR_XIR_OK);
    CHECK(effect_source_contexts(context,&current,effects)==XR_XIR_OK);
    CHECK(effect_source_publication_match(context,&current,effects)==XR_XIR_OK);
    size_t prefix_allocations=attempts-before,prefix_frees=pc_source_terminal_free_count;pc_free_hook=NULL;
    size_t prefix=mutation==2?prefix_frees:prefix_allocations;
    CHECK(!pc_allocate_hook && !pc_source_terminal_row && mutation<3);
    pc_source_terminal_skip=prefix;pc_source_terminal_observed=0;pc_source_terminal_calls=0;
    pc_source_terminal_mutation=mutation;pc_source_terminal_row=&rows[0];
    if (mutation==2) pc_free_hook=pc_source_terminal_trigger;else pc_allocate_hook=pc_source_terminal_trigger;
    XrXirStatus status=xir_effects_source_terminal(context,&current,effects);
    pc_allocate_hook=NULL;pc_free_hook=NULL;pc_source_terminal_row=NULL;
    CHECK(pc_source_terminal_calls==1 && pc_source_terminal_observed==prefix+1);
    rows[0].type=XR_XIR_I64;
    CHECK(xir_effects_source_terminal(context,&current,effects)==XR_XIR_OK);
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

static void pc_source_terminal_literals(void) {
    for (uint32_t mutation=0;mutation<3;++mutation) pc_source_terminal_literal(mutation);
}
