/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_capture_terminal.c - Complete receiving after resource cleanup
 *
 * KEY CONCEPT:
 *   Outer and nested capture receivers independently reject inconsistent facts.
 *   Physical allocation and release counters span both complete terminal chains.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "base/xmalloc.h"
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t allocations,releases,blocks,seen_releases,attack_cut,attacks;
static void (*release_hook)(void);
static void *capture_malloc(size_t bytes) {
    void *p=xr_malloc(bytes);if(p){++allocations;++blocks;}return p;
}
static void capture_free(void *p) {
    if(!p)return;
    ++releases;CHECK(blocks);--blocks;
    if(release_hook)release_hook();
    xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) capture_malloc(bytes)
#define xr_free(p) capture_free(p)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#ifdef interface
#undef interface
#endif
#include "xir/xxir_effects.c"
#include "xir_construction_fixture.h"
static EffectInvocationFinal *attack_final;
static uint32_t mutation_kind;
static void unsupported_capture(EffectInvocationFinal *final) {
    EffectCaptureFact fact={final->roots[5],2,0,final->site_count+3,0};
    CHECK(final->capture.count<final->capture.capacity);uint32_t at=0;
    while(at<final->capture.count&&effect_capture_before(&final->capture.facts[at],&fact))++at;
    memmove(final->capture.facts+at+1,final->capture.facts+at,
        (size_t)(final->capture.count-at)*sizeof(fact));
    final->capture.facts[at]=fact;++final->capture.count;
}
static void last_release_attack(void) {
    if(++seen_releases!=attack_cut)return;
    release_hook=NULL;CHECK(attack_final);
    switch(mutation_kind) {
    case 0:unsupported_capture(attack_final);break;
    case 1:attack_final->capture.count=0;break;
    case 2:CHECK(attack_final->capture.facts[0].rank+1<attack_final->capture.count);
        ++attack_final->capture.facts[0].rank;break;
    case 3:CHECK(attack_final->material->functions[1].runs[0].base.op==XR_XIR_CONST_INT);
        attack_final->material->functions[1].runs[0].base.type=XR_XIR_BOOL;break;
    default:CHECK(attack_final->edge_count);attack_final->edges[0].target=attack_final->count;break;
    }
    ++attacks;
}
static XrXirStatus terminal(const XrXirCompileContext *context,XrXirArtifact *artifact,XrXirEffects *effects,bool source) {
    (void)artifact;
    return source?effect_source_terminal_roles(context,effects):
        effect_final_inventories_match(context,effects->invocations,effects->invocations->final->outcomes);
}
static void fixture(bool source) {
    XrXirTypeNode type={.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE,.element=XR_XIR_NEVER};
    XrXirTypes types={.nodes=&type,.count=1};XrXirType scalar=XR_XIR_I64,callback=(XrXirType)256;
    XrXirInstruction ret={.op=XR_XIR_RETURN};uint32_t operands[2]={0,0},actuals[2]={0,1};
    XrXirInstruction ops[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={1,1},.immediate=2},
        {.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction caller[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=XR_XIR_CALL,.type=XR_XIR_UNIT,.args={1,1},.immediate=3},{.op=XR_XIR_RETURN}};
    uint32_t loop_operands[4]={0,0,1,2};
    XrXirInstruction loop[5]={{.op=XR_XIR_JUMP,.targets={1}},
        {.op=XR_XIR_PHI,.type=(XrXirType)256,.args={0,4}},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=0},
        {.op=XR_XIR_BRANCH,.args={3},.targets={1,2}},{.op=XR_XIR_RETURN}};
    XrXirBlock bs[8]={{.count=1},{.count=4},{.count=1},{.count=1},{.count=4},
        {.count=1},{.first=1,.count=3},{.first=4,.count=1}};
    XrXirFunction fs[6]={
        {.name="init",.name_length=4,.instructions=&ret,.instruction_count=1,.blocks=bs,.block_count=1},
        {.name="factory",.name_length=7,.result=(XrXirType)256,.instructions=ops,.instruction_count=4,
            .blocks=bs+1,.block_count=1,.operands=operands,.operand_count=2},
        {.name="scalar",.name_length=6,.parameters=&scalar,.parameter_count=1,.instructions=&ret,
            .instruction_count=1,.blocks=bs+2,.block_count=1},
        {.name="callback",.name_length=8,.parameters=&callback,.parameter_count=1,.instructions=&ret,
            .instruction_count=1,.blocks=bs+3,.block_count=1},
        {.name="caller",.name_length=6,.instructions=caller,.instruction_count=4,.blocks=bs+4,
            .block_count=1,.operands=actuals,.operand_count=2},
        {.name="cycle",.name_length=5,.parameters=&callback,.parameter_count=1,.instructions=loop,
            .instruction_count=5,.blocks=bs+5,.block_count=3,.operands=loop_operands,.operand_count=4}};
    for(unsigned f=0;f<6;++f)fs[f].error_type=XR_XIR_NEVER;
    const XrXirSourceModule m={.name="edges",.name_length=5,.initializer=0,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirFunctionIdentity ids[6]={0};
    const XrXirDeclarations ds={.modules=&m,.module_count=1,.functions=ids,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirEffectParameter plain={0},variable={.kind=XR_XIR_EFFECT_PARAMETER_VARIABLE},
        cycle={.kind=XR_XIR_EFFECT_PARAMETER_VARIABLE,.uses=XR_XIR_EFFECT_USE_COPY};
    XrXirFunctionEffectContract cs[6]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0},
        {.parameter_count=1,.parameters=&plain},{.parameter_count=1,.parameters=&variable},{0},
        {.parameter_count=1,.parameters=&cycle}};
    const XrXirProvenance provenance={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=cs,.contract_count=6};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_LIBRARY,.functions=fs,.function_count=6,
        .types=&types,.declarations=&ds,.provenance=&provenance};
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0};
    CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&module,&artifact,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"fixture status%u f%u b%u i%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK&&artifact);
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK&&effects);
    CHECK(effect_source_terminal_roles(&context,effects)==XR_XIR_OK);
    EffectInvocationFinal *outer=effects->invocations->final;
    CHECK(outer->outcomes->contexts && outer->outcomes->contexts->forest);
    EffectInvocationFinal *targets[2]={outer,outer->outcomes->contexts->forest->invocations->final};
    for(uint32_t target=0;target<2;++target) {
        EffectInvocationFinal *final=targets[target];
        CHECK(final->count>=6 && final->site_count==3);
        XrXirStatus allocated=XR_XIR_OK;
        uint32_t count=final->capture.count;CHECK(count>1);
        EffectCaptureFact *replacement=xir_compile_alloc(&context,((size_t)count+1)*sizeof(*replacement),&allocated);
        CHECK(allocated==XR_XIR_OK&&replacement);
        memcpy(replacement,final->capture.facts,(size_t)count*sizeof(*replacement));
        xr_compile_resources_free(final->capture.facts);
        final->capture.facts=replacement;final->capture.capacity=count+1;
        EffectCaptureFact *saved=xr_malloc((size_t)count*sizeof(*saved));CHECK(saved);
        memcpy(saved,replacement,(size_t)count*sizeof(*saved));
        size_t a=allocations,f=releases;
        CHECK(terminal(&context,artifact,effects,source)==XR_XIR_OK);
        CHECK(allocations==a&&releases==f);
        EffectBodyRun *run=&final->material->functions[1].runs[0];
        XrXirInstruction original_input=run->base;
        CHECK(final->edge_count);EffectInvocationEdge original_edge=final->edges[0];
        for(uint32_t mutation=0;mutation<5;++mutation) {
            void *retired=NULL;
            CHECK(xr_compile_resources_alloc(context.resources,1,&retired)==XR_COMPILE_RESOURCE_OK);
            attack_final=final;mutation_kind=mutation;attack_cut=1;
            seen_releases=attacks=0;release_hook=last_release_attack;
            xr_compile_resources_free(retired);
            CHECK(attacks==1&&!release_hook);
            if(mutation<3) {
                EffectCaptureRead frame={.work=&context,.final=final,.fallback=true,.fixed_only=true,.class_results=true};
                CHECK(effect_capture_frame(&frame)==XR_XIR_OK);
            }
            a=allocations;f=releases;
            status=terminal(&context,artifact,effects,source);
            CHECK(status==XR_XIR_BAD_STRUCTURE);
            CHECK(allocations==a&&releases==f);
            XrXirStatus public_status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(artifact),effects);
            CHECK(public_status==XR_XIR_BAD_STRUCTURE);
            final->capture.count=count;memcpy(replacement,saved,(size_t)count*sizeof(*saved));
            run->base=original_input;final->edges[0]=original_edge;
            CHECK(terminal(&context,artifact,effects,source)==XR_XIR_OK);
            CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(artifact),effects)==XR_XIR_OK);
            fprintf(stderr,"terminal%u target%u mutation%u: cleanup observed, rejected status%u, restored PASS\n",
                source,target,mutation,status);
        }
        f=releases;
        CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(artifact),effects)==XR_XIR_OK);
        size_t prefix_frees=releases-f;CHECK(prefix_frees);
        attack_final=final;mutation_kind=0;attack_cut=prefix_frees;
        seen_releases=attacks=0;release_hook=last_release_attack;
        status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(artifact),effects);
        release_hook=NULL;
        CHECK(attacks==1 && seen_releases==prefix_frees && final->capture.count==count+1);
        CHECK(status==XR_XIR_BAD_STRUCTURE);
        final->capture.count=count;memcpy(replacement,saved,(size_t)count*sizeof(*saved));
        CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(artifact),effects)==XR_XIR_OK);
        fprintf(stderr,"terminal%u target%u: public Source prefix last release%zu changed sorted capture, final rejected, restored PASS\n",
            source,target,prefix_frees);
        xr_free(saved);attack_final=NULL;
        fprintf(stderr,"terminal%u target%u: cleanup mutation observed, sorted unsupported fact rejected, retry accepted\n",
            source,target);
    }
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(artifact);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK&&stats.held_bytes==initial.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!blocks);
}
int main(void) {
    fixture(false);fixture(true);
    puts("Complete capture terminals: outer/nested callback-free receivers reject cleanup mutation PASS");
    return 0;
}
