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
static bool fail_malloc;
static void (*release_hook)(void);
static void *capture_malloc(size_t bytes) {
    if(fail_malloc)return NULL;
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
static uint32_t mutation_kind,routing_caller,routing_other_head;
static uint64_t capture_held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static void routing_select(const EffectInvocationFinal *final) {
    routing_caller=UINT32_MAX;routing_other_head=UINT32_MAX;
    CHECK(final->edge_count>1);
    for(uint32_t n=0;n<final->count;++n) {
        uint32_t head=final->nodes[n].edge_head;
        if(head==UINT32_MAX) continue;
        CHECK(head<final->edge_count && final->edges[head].caller==n);
        if(routing_caller==UINT32_MAX) routing_caller=n;
        else { routing_other_head=head;break; }
    }
    CHECK(routing_caller<final->count && routing_other_head<final->edge_count);
}
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
    case 4:CHECK(attack_final->edge_count);attack_final->edges[0].target=attack_final->count;break;
    case 5: {
        uint32_t head=attack_final->nodes[routing_caller].edge_head;
        attack_final->nodes[routing_caller].edge_head=attack_final->edges[head].next;break;
    }
    case 6:attack_final->nodes[routing_caller].edge_head=routing_other_head;break;
    case 7:attack_final->edges[0].next=0;break;
    case 8:attack_final->edges[0].next=1;break;
    case 9:attack_final->nodes[routing_caller].edge_head=attack_final->edge_count;break;
    default:CHECK(false);break;
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
        routing_select(final);
        uint32_t original_head=final->nodes[routing_caller].edge_head;
        for(uint32_t mutation=0;mutation<10;++mutation) {
            uint64_t held=capture_held(&context);
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
            XrXirStatus public_status=xir_effects_source_terminal_input(&context,(XirEffectInput){xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL},effects);
            CHECK(public_status==XR_XIR_BAD_STRUCTURE);
            CHECK(capture_held(&context)==held);
            final->capture.count=count;memcpy(replacement,saved,(size_t)count*sizeof(*saved));
            run->base=original_input;final->edges[0]=original_edge;
            final->nodes[routing_caller].edge_head=original_head;
            a=allocations;f=releases;
            CHECK(terminal(&context,artifact,effects,source)==XR_XIR_OK);
            CHECK(allocations==a&&releases==f);
            CHECK(xir_effects_source_terminal_input(&context,(XirEffectInput){xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL},effects)==XR_XIR_OK);
            CHECK(capture_held(&context)==held);
            fprintf(stderr,"terminal%u target%u mutation%u: cleanup observed, rejected status%u, restored PASS\n",
                source,target,mutation,status);
        }
        f=releases;
        CHECK(xir_effects_source_terminal_input(&context,(XirEffectInput){xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL},effects)==XR_XIR_OK);
        size_t prefix_frees=releases-f;CHECK(prefix_frees);
        attack_final=final;mutation_kind=0;attack_cut=prefix_frees;
        seen_releases=attacks=0;release_hook=last_release_attack;
        status=xir_effects_source_terminal_input(&context,(XirEffectInput){xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL},effects);
        release_hook=NULL;
        CHECK(attacks==1 && seen_releases==prefix_frees && final->capture.count==count+1);
        CHECK(status==XR_XIR_BAD_STRUCTURE);
        final->capture.count=count;memcpy(replacement,saved,(size_t)count*sizeof(*saved));
        CHECK(xir_effects_source_terminal_input(&context,(XirEffectInput){xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL},effects)==XR_XIR_OK);
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
typedef struct CaptureClassFixture {
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
    XrXirEffectParameter plain;
    XrXirFunctionEffectContract contracts[5];
    XrXirProvenance provenance;
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} CaptureClassFixture;

/* These literal graphs distinguish a constructor, PHI, local writes, Cell
 * transport and recursion without deriving the expected masks from a solver. */
static void capture_class_fixture(CaptureClassFixture *f,uint32_t mode) {
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
    if(mode==1 || mode==4) {
        f->entry[2]=mode==1?
            (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7}:
            (XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
        f->entry[3]=mode==1?
            (XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=1}:
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
        if(mode==4) {
            f->entry[8].args[0]=1;f->entry[9].args[0]=5;
            f->entry_operands[1]=1;f->entry_operands[2]=1;
            f->entry_operands[3]=2;f->entry_operands[4]=3;f->entry_operands[5]=8;
        }
        f->entry_blocks[0]=(XrXirBlock){0,6,0,0};f->entry_blocks[1]=(XrXirBlock){6,1,0,0};
        f->entry_blocks[2]=(XrXirBlock){7,1,0,0};f->entry_blocks[3]=(XrXirBlock){8,3,0,0};
        entry_count=11;operand_count=mode==4?6:7;block_count=4;
    } else if(mode==2 || mode==6) {
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=object,.args={1}};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=1};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={2,4}};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=object,.args={2}};
        f->entry[7]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={2,1},.immediate=2};
        f->entry[8]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={7}};
        f->entry_operands[1]=3;f->entry_operands[2]=6;entry_count=9;operand_count=3;
        if(mode==6) {
            f->entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
            f->entry[4]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={3}};
            f->entry[7].args[0]=1;f->entry_operands[1]=6;operand_count=2;
        }
    } else if(mode==3) {
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={2}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={1,1},.immediate=2};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};
        f->entry_operands[1]=3;entry_count=6;
    } else if(mode==5) {
        f->forward[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={0,1},.immediate=2};
        f->forward[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};forward_count=2;
    }
    f->init_block=(XrXirBlock){0,1,0,0};f->constructor_block=(XrXirBlock){0,2,0,0};
    f->forward_block=(XrXirBlock){0,forward_count,0,0};
    if(block_count==1) f->entry_blocks[0]=(XrXirBlock){0,entry_count,0,0};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->init_block,.block_count=1,.instructions=f->init,.instruction_count=1,.error_type=XR_XIR_NEVER};
    f->functions[1]=(XrXirFunction){.name="new",.name_length=3,.parameters=&f->constructor_parameter,
        .parameter_count=1,.result=object,.blocks=&f->constructor_block,.block_count=1,
        .instructions=f->constructor,.instruction_count=2,.operands=f->constructor_operands,.operand_count=1,.error_type=XR_XIR_NEVER};
    f->functions[2]=(XrXirFunction){.name="forward",.name_length=7,.parameters=&f->forward_parameter,
        .parameter_count=1,.result=object,.blocks=&f->forward_block,.block_count=1,
        .instructions=f->forward,.instruction_count=forward_count,.operands=mode==5?f->forward_operands:NULL,
        .operand_count=mode==5?1u:0u,.error_type=XR_XIR_NEVER};
    f->functions[3]=(XrXirFunction){.name="entry",.name_length=5,.result=object,
        .blocks=f->entry_blocks,.block_count=block_count,.instructions=f->entry,.instruction_count=entry_count,
        .operands=f->entry_operands,.operand_count=operand_count,.error_type=XR_XIR_NEVER};
    f->main[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.immediate=3};
    f->main[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->main[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->main_block=(XrXirBlock){0,3,0,0};
    f->functions[4]=(XrXirFunction){.name="main",.name_length=4,.result=XR_XIR_I64,
        .blocks=&f->main_block,.block_count=1,.instructions=f->main,.instruction_count=3,.error_type=XR_XIR_NEVER};
    f->identities[1]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=0,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=4};
    f->contracts[0].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[1]=(XrXirFunctionEffectContract){.parameters=&f->plain,.parameter_count=1};
    f->contracts[2]=(XrXirFunctionEffectContract){.parameters=&f->plain,.parameter_count=1};
    f->provenance=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=5};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM,.provenance=&f->provenance};
}

static EffectCaptureFact *capture_class_fact(const XrXirCompileContext *context,
    EffectInvocationFinal *final,uint32_t index,uint32_t value,uint32_t flag) {
    EffectCaptureRead read={.work=context,.final=final,.body=effect_body_owned_request(context,final->material)};
    EffectCaptureNode node={0};CHECK(effect_capture_node(&read,index,&node)==XR_XIR_OK && flag<3);
    uint32_t bit=node.basis.parameters+flag;const EffectCaptureFact *fact=NULL;
    CHECK(effect_capture_find(&read,(EffectCaptureFact){index,value,node.basis.fn_words+bit/64,bit%64,0},&fact)==XR_XIR_OK);
    return (EffectCaptureFact *)fact;
}
static void capture_class_remove(EffectCaptureProof *proof,EffectCaptureFact *fact) {
    CHECK(fact && fact>=proof->facts && fact<proof->facts+proof->count);
    uint32_t at=(uint32_t)(fact-proof->facts);
    memmove(fact,fact+1,(size_t)(proof->count-at-1)*sizeof(*fact));--proof->count;
}
static void capture_class_mask(const XrXirCompileContext *context,EffectInvocationFinal *final,
    uint32_t index,uint32_t value,uint32_t mask) {
    EffectCaptureFact *known=capture_class_fact(context,final,index,value,2);CHECK(known);
    uint64_t known_rank=known->rank;
    for(uint32_t flag=0;flag<3;++flag) {
        EffectCaptureFact *fact=capture_class_fact(context,final,index,value,flag);
        if(fact) capture_class_remove(&final->capture,fact);
    }
    EffectCaptureRead read={.work=context,.final=final,.body=effect_body_owned_request(context,final->material)};
    EffectCaptureNode node={0};CHECK(effect_capture_node(&read,index,&node)==XR_XIR_OK);
    for(uint32_t flag=0;flag<3;++flag) {
        uint32_t present=flag==0?EFFECT_CLASS_ROOT:flag==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
        if(!(mask&present)) continue;
        uint32_t bit=node.basis.parameters+flag,at=0;
        EffectCaptureFact fact={index,value,node.basis.fn_words+bit/64,bit%64,flag==2?known_rank:0};
        CHECK(final->capture.count<final->capture.capacity && effect_capture_lower(&read,&fact,&at)==XR_XIR_OK);
        memmove(final->capture.facts+at+1,final->capture.facts+at,(size_t)(final->capture.count-at)*sizeof(fact));
        final->capture.facts[at]=fact;++final->capture.count;
    }
}
static void capture_class_rejections(const XrXirCompileContext *context,XrXirArtifact *artifact,
    XrXirEffects *effects,uint32_t node,uint32_t value,uint32_t expected) {
    EffectInvocationFinal *final=effects->invocations->final;EffectCaptureProof saved=final->capture;
    CHECK(saved.count<UINT32_MAX);XrXirStatus status=XR_XIR_OK;
    EffectCaptureFact *facts=xir_compile_alloc(context,((size_t)saved.count+1)*sizeof(*facts),&status);
    CHECK(facts && status==XR_XIR_OK);
    final->capture=(EffectCaptureProof){facts,saved.count,saved.count+1};
    XirEffectInput input={xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),NULL};
    for(uint32_t mutation=0;mutation<3;++mutation) {
        memcpy(facts,saved.facts,(size_t)saved.count*sizeof(*facts));final->capture.count=saved.count;
        uint64_t held=capture_held(context);
        if(!mutation) {
            uint32_t counterfactual=expected==1?5u:1u,mask=0;
            capture_class_mask(context,final,node,value,counterfactual);
            CHECK(effect_final_class_mask(context,final,node,value,&mask)==XR_XIR_OK && mask==counterfactual);
        } else {
            EffectCaptureFact *known=capture_class_fact(context,final,node,value,2);CHECK(known);
            if(mutation==1) capture_class_remove(&final->capture,known);
            else known->rank=UINT32_MAX;
        }
        size_t a=allocations,f=releases;
        CHECK(terminal(context,artifact,effects,false)==XR_XIR_BAD_STRUCTURE);
        CHECK(terminal(context,artifact,effects,true)==XR_XIR_BAD_STRUCTURE);
        CHECK(allocations==a && releases==f);
        CHECK(xir_effects_source_terminal_input(context,input,effects)==XR_XIR_BAD_STRUCTURE);
        CHECK(capture_held(context)==held);
        memcpy(facts,saved.facts,(size_t)saved.count*sizeof(*facts));final->capture.count=saved.count;
        a=allocations;f=releases;
        CHECK(terminal(context,artifact,effects,false)==XR_XIR_OK);
        CHECK(terminal(context,artifact,effects,true)==XR_XIR_OK);
        CHECK(allocations==a && releases==f);
        CHECK(xir_effects_source_terminal_input(context,input,effects)==XR_XIR_OK);
        CHECK(capture_held(context)==held);
    }
    final->capture=saved;xr_compile_resources_free(facts);
}
static void capture_class_case(uint32_t mode) {
    static const uint32_t values[7]={2,9,7,4,9,2,7},masks[7]={1,1,1,5,5,5,5};
    CaptureClassFixture literal;capture_class_fixture(&literal,mode);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t baseline=capture_held(&context);
    XrXirArtifact *artifact=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&literal.module,&artifact,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"Class mode%u check status%u f%u b%u i%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && artifact);
    CHECK(xr_xir_compile_artifact_verify(artifact,&diagnostic)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    CHECK(terminal(&context,artifact,effects,false)==XR_XIR_OK);
    CHECK(terminal(&context,artifact,effects,true)==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;
    uint32_t selected=final->roots[3],output=0;CHECK(final->capture.count);
    size_t a=allocations,f=releases;
    CHECK(xir_effects_class_value(&context,effects,selected,3,values[mode],&output)==XR_XIR_OK && output==masks[mode]);
    output=0;CHECK(xir_effects_class_value(&context,effects,final->roots[2],2,0,&output)==XR_XIR_OK && output==5);
    uint32_t factory=final->roots[1],factory_values=final->nodes[factory].parameter_count+final->nodes[factory].instruction_count;
    output=0;CHECK(effect_final_class_mask(&context,final,factory,factory_values,&output)==XR_XIR_OK && output==1);
    uint32_t returned=final->nodes[selected].parameter_count+final->nodes[selected].instruction_count;
    output=0;CHECK(effect_final_class_mask(&context,final,selected,returned,&output)==XR_XIR_OK && output==masks[mode]);
    if(mode==6) { output=0;CHECK(xir_effects_class_value(&context,effects,selected,3,2,&output)==XR_XIR_OK && output==5); }
    CHECK(allocations==a && releases==f);
    capture_class_rejections(&context,artifact,effects,selected,values[mode],masks[mode]);
    xr_xir_compile_artifact_free(artifact);artifact=NULL;memset(&literal,0xCE,sizeof(literal));
    a=allocations;f=releases;
    CHECK(terminal(&context,artifact,effects,false)==XR_XIR_OK);
    CHECK(terminal(&context,artifact,effects,true)==XR_XIR_OK);
    CHECK(allocations==a && releases==f);
    xr_xir_compile_effects_free(effects);CHECK(capture_held(&context)==baseline);
    xr_compile_resources_release(context.resources);CHECK(!blocks);
    fprintf(stderr,"Class mode%u value%u mask%u: mask/known/rank rejected, restored, producer retired PASS\n",
        mode,values[mode],masks[mode]);
}
static void capture_schedule_index(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t baseline=capture_held(&context);
    EffectInvocationEdge edges[]={
        {.caller=0,.target=1},{.caller=2,.target=1},{.caller=1,.target=1},
        {.caller=3,.target=0},{.caller=0,.target=1}
    };
    EffectInvocationFinal final={.count=5,.edge_count=5,.edges=edges};
    EffectCaptureRead read={.work=&context,.final=&final};
    EffectCaptureSchedule *schedule=NULL;
    fail_malloc=true;
    CHECK(effect_capture_schedule_create(&read,&schedule)==XR_XIR_OUT_OF_MEMORY && !schedule);
    fail_malloc=false;CHECK(capture_held(&context)==baseline);
    CHECK(effect_capture_schedule_create(&read,&schedule)==XR_XIR_OK && schedule);
    const uint64_t offsets[]={0,3,8,9,10,10};
    const uint32_t incidents[]={0,3,4,0,1,2,2,4,1,3},jobs[]={1,5,6,7,9};
    CHECK(!memcmp(schedule->incident_offsets,offsets,sizeof(offsets)));
    CHECK(!memcmp(schedule->incident_edges,incidents,sizeof(incidents)));
    CHECK((uintptr_t)schedule->incident_offsets%_Alignof(uint64_t)==0);
    CHECK((uintptr_t)schedule->queue%_Alignof(uint32_t)==0);
    read.schedule=schedule;
    effect_capture_schedule_next_job(schedule);
    CHECK(effect_capture_schedule_changed(&read,1)==XR_XIR_OK && schedule->count==5);
    CHECK(!memcmp(schedule->queue,jobs,sizeof(jobs)));
    CHECK(effect_capture_schedule_changed(&read,1)==XR_XIR_OK && schedule->count==5);
    const uint32_t popped[]={1,5};
    for(unsigned i=0;i<2;++i) {
        uint32_t job=schedule->queue[schedule->front];CHECK(job==popped[i]);
        schedule->pending[job]=0;
        schedule->front=schedule->front+1==schedule->capacity?0:schedule->front+1;
        --schedule->count;effect_capture_schedule_next_job(schedule);
        CHECK(effect_capture_schedule_changed(&read,1)==XR_XIR_OK && schedule->count==5);
        CHECK(schedule->pending[1] && schedule->pending[5]);
    }
    schedule->epoch=UINT64_MAX;
    effect_capture_schedule_next_job(schedule);
    CHECK(schedule->epoch==1);
    for(uint32_t n=0;n<schedule->nodes;++n) CHECK(!schedule->change_epochs[n]);
    CHECK(effect_capture_schedule_changed(&read,4)==XR_XIR_OK && schedule->count==6 && schedule->pending[4]);
    read.schedule=NULL;xr_compile_resources_free(schedule);schedule=NULL;
    CHECK(capture_held(&context)==baseline);
    edges[0].target=final.count;
    CHECK(effect_capture_schedule_create(&read,&schedule)==XR_XIR_BAD_STRUCTURE && !schedule);
    CHECK(capture_held(&context)==baseline);
    final.count=3;final.edge_count=0;final.edges=NULL;
    CHECK(effect_capture_schedule_create(&read,&schedule)==XR_XIR_OK && schedule);
    CHECK(!schedule->incident_offsets[3]);
    read.schedule=schedule;effect_capture_schedule_next_job(schedule);
    CHECK(effect_capture_schedule_changed(&read,2)==XR_XIR_OK && schedule->count==1 && schedule->queue[0]==2);
    read.schedule=NULL;xr_compile_resources_free(schedule);
    CHECK(capture_held(&context)==baseline);
    xr_compile_resources_release(context.resources);CHECK(!blocks);
}
int main(void) {
    capture_schedule_index();
    fixture(false);fixture(true);
    for(uint32_t mode=0;mode<7;++mode) capture_class_case(mode);
    puts("Complete capture terminals: outer/nested callback-free receivers reject cleanup mutation PASS");
    return 0;
}
