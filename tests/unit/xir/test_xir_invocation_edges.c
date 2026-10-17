/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_invocation_edges.c - Actual edge identity after the final receiving free
 */
#define main edges_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static EffectInvocationFinal *edges_final;
static bool edges_no_return,edges_opaque,edges_large,edges_faults;
static uint32_t edges_mask;
static uint32_t edges_frees,edges_cut,edges_mode,edges_attacks,edges_direct,edges_reference;
static EffectCaptureFact *edges_capture_identity(EffectInvocationFinal *final,uint32_t node,uint32_t value) {
    for(uint32_t p=0;p<final->capture.count;++p) {
        EffectCaptureFact *fact=&final->capture.facts[p];
        if(fact->node==node && fact->value==value && fact->word==0 && fact->bit==2) return fact;
    }
    CHECK(false);return NULL;
}
static void edges_capture_erase(EffectInvocationFinal *final,uint32_t node,uint32_t value,uint32_t word,uint32_t bit) {
    for(uint32_t p=0;p<final->capture.count;++p) {
        EffectCaptureFact *fact=&final->capture.facts[p];
        if(fact->node==node && fact->value==value && fact->word==word && fact->bit==bit) {
            --final->capture.count;
            memmove(fact,fact+1,(size_t)(final->capture.count-p)*sizeof(*fact));return;
        }
    }
    CHECK(false);
}
/* Counterfactual contents retain the canonical envelope, so closure and
 * positive support remain necessary after the structural proof succeeds. */
static void edges_capture_fake(EffectInvocationFinal *final,EffectCaptureFact fact) {
    CHECK(final->capture.count<final->capture.capacity);uint32_t at=0;
    while(at<final->capture.count && effect_capture_before(&final->capture.facts[at],&fact)) ++at;
    memmove(final->capture.facts+at+1,final->capture.facts+at,
        (size_t)(final->capture.count-at)*sizeof(*final->capture.facts));
    final->capture.facts[at]=fact;++final->capture.count;
}
static void edges_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);++edges_frees;
    if(edges_frees!=edges_cut || !edges_final) return;
    pc_free_hook=NULL;++edges_attacks;EffectInvocationFinal *final=edges_final;
    EffectInvocationEdge *direct=&final->edges[edges_direct],*reference=&final->edges[edges_reference];
    switch(edges_mode) {
    case 1: direct->caller=0;break;
    case 2: direct->target=final->roots[3];break;
    case 3: direct->instruction=3;break;
    case 4: direct->producer=0;break;
    case 5: direct->latent=true;break;
    case 6: direct->next=edges_direct;break;
    case 7: final->nodes[final->roots[4]].edge_head=UINT32_MAX;break;
    case 8: direct->target=final->count;break;
    case 9: final->edges=(void *)(uintptr_t)1;break;
    case 10: final->edge_count=0;final->edges=NULL;break;
    case 11:
        CHECK(final->edge_count<final->edge_capacity);
        final->edges[final->edge_count]=*direct;final->edges[final->edge_count].next=edges_direct;
        final->nodes[direct->caller].edge_head=final->edge_count++;break;
    case 12: reference->producer=0;break;
    case 13: reference->latent=false;break;
    case 14: reference->consumer_next=edges_reference;break;
    case 15: CHECK(reference->consumer_next!=UINT32_MAX);reference->consumer_next=UINT32_MAX;break;
    case 16:
        CHECK(edges_direct==final->edge_count-1);
        final->nodes[direct->caller].edge_head=direct->next;--final->edge_count;break;
    case 17:
        CHECK(edges_direct==final->edge_count-1 && direct->target==final->count-1);
        final->nodes[direct->caller].edge_head=direct->next;--final->edge_count;--final->count;break;
    case 18: final->nodes[direct->target].input[0]=0;break;
    case 19: final->nodes[direct->target].input[0]=0;edges_capture_erase(final,direct->target,0,0,2);break;
    case 20:
        final->nodes[direct->target].input[0]=0;
        for(uint32_t p=0;p<final->capture.count;) {
            EffectCaptureFact *fact=&final->capture.facts[p];
            if(fact->word!=EFFECT_CAPTURE_FLAGS && (uint64_t)fact->word*64+fact->bit<final->site_count) {
                --final->capture.count;memmove(fact,fact+1,(size_t)(final->capture.count-p)*sizeof(*fact));
            } else ++p;
        }
        break;
    case 21: edges_capture_identity(final,direct->target,0)->rank=UINT64_MAX;break;
    case 22: CHECK(edges_capture_identity(final,direct->target,0)->rank);edges_capture_identity(final,direct->target,0)->rank=0;break;
    case 23: {
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,*edges_capture_identity(final,direct->target,0));break;
    }
    case 24: edges_capture_identity(final,direct->target,0)->bit=0;break;
    case 25: edges_capture_identity(final,direct->caller,1)->bit=0;break;
    case 26: edges_capture_identity(final,direct->caller,1)->value=0;break;
    case 27: final->capture.capacity=UINT32_MAX;break;
    case 28: final->capture.facts=(void *)(uintptr_t)1;break;
    case 29: final->capture.count=final->capture.capacity+1;break;
    case 30: {
        edges_capture_fake(final,(EffectCaptureFact){final->roots[5],2,0,2,1});break;
    }
    case 31: final->nodes[direct->target].input[0]=0;break;
    case 32: final->nodes[direct->target].input[0]=8;break;
    case 33: final->nodes[direct->target].input[0]=16;break;
    case 34: final->nodes[direct->target].input[0]=0;final->capture.count=0;break;
    case 35: final->capture.count=0;break;
    case 36: CHECK(final->capture.count<final->capture.capacity);final->capture.facts[final->capture.count++]=final->capture.facts[0];break;
    case 37: final->capture.facts[0].rank=UINT64_MAX;break;
    case 38: final->capture.facts[0].node=final->count;break;
    case 39: final->capture.facts[0].value=UINT32_MAX;break;
    case 40: final->capture.facts[0].bit=UINT32_MAX-2;break;
    case 41: final->capture.facts=(void *)(uintptr_t)1;break;
    case 42: final->capture.count=final->capture.capacity+1;break;
    case 43:
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,(EffectCaptureFact){final->roots[5],2,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,1});break;
    case 44: edges_capture_erase(final,direct->caller,2,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW);break;
    case 45:
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,(EffectCaptureFact){final->roots[1],1,0,final->site_count+2,1});break;
    case 46:
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,(EffectCaptureFact){final->roots[5],2,0,final->site_count,1});break;
    case 47:
        final->capture.count=0;
        for(uint32_t n=0;n<final->count;++n) if(final->nodes[n].input) final->nodes[n].input[0]=0;
        break;
    case 48:case 49: final->nodes[direct->target].input[0]=0;break;
    case 50: final->nodes[direct->target].input[0]=24;break;
    case 51: final->nodes[direct->target].input[0]=32;break;
    case 52: {
        bool found=false;
        for(uint32_t p=0;p<final->capture.count;++p) {
            EffectCaptureFact *fact=&final->capture.facts[p];
            if(fact->node==final->roots[5] && fact->value==2 && fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_RAW) {
                CHECK(fact->rank==1);fact->rank=0;found=true;break;
            }
        }
        CHECK(found);break;
    }
    case 53: edges_capture_erase(final,final->roots[5],0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW);break;
    case 54:
        edges_capture_erase(final,final->roots[5],2,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW);
        edges_capture_erase(final,final->roots[5],2,0,final->site_count+1);
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,(EffectCaptureFact){final->roots[5],2,0,final->site_count+3,0});break;
    case 55:
        CHECK(final->capture.count<final->capture.capacity);
        edges_capture_fake(final,(EffectCaptureFact){final->roots[5],2,0,final->site_count+3,0});break;
    case 56: {
        CHECK(final->capture.count>1);EffectCaptureFact first=final->capture.facts[0];
        final->capture.facts[0]=final->capture.facts[1];final->capture.facts[1]=first;break;
    }
    case 57:
        for(uint32_t p=0;p<final->capture.count;) {
            EffectCaptureFact *fact=&final->capture.facts[p];
            if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_COMPLETE) {
                --final->capture.count;memmove(fact,fact+1,(size_t)(final->capture.count-p)*sizeof(*fact));
            } else ++p;
        }
        break;
    case 58: {
        bool found=false;
        for(uint32_t p=0;p<final->capture.count;++p) {
            EffectCaptureFact *fact=&final->capture.facts[p];
            if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_COMPLETE && fact->rank) {
                fact->rank=0;found=true;break;
            }
        }
        CHECK(found);break;
    }
    case 59: edges_capture_fake(final,(EffectCaptureFact){final->roots[5],0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});break;
    case 60: edges_capture_fake(final,(EffectCaptureFact){final->roots[4],2,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});break;
    default: CHECK(false);
    }
}
static void edges_forbid(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static void edges_capture_equal(const EffectInvocationFinal *actual,const EffectInvocationFinal *expected) {
    CHECK(actual->capture.count==expected->capture.count);
    for(uint32_t p=0;p<actual->capture.count;++p) {
        const EffectCaptureFact *left=&actual->capture.facts[p],*right=&expected->capture.facts[p];
        CHECK(left->node==right->node && left->value==right->value && left->word==right->word && left->bit==right->bit && left->rank==right->rank);
    }
}
static void edges_capture_faults(const XrXirCompileContext *work,EffectInvocationFinal *final) {
    uint64_t outer_live=pc_stats(work).live_bytes;size_t outer_physical=live_bytes,outer_blocks=live;
    XrXirStatus allocated_status=XR_XIR_OK;
    EffectInvocationFinal *staged=xir_compile_alloc(work,sizeof(*staged),&allocated_status);
    CHECK(staged && allocated_status==XR_XIR_OK);*staged=*final;staged->capture=(EffectCaptureProof){0};
    uint64_t baseline=pc_stats(work).live_bytes;size_t physical=live_bytes,blocks=live,before=attempts;
    CHECK(effect_capture_build(work,staged)==XR_XIR_OK);
    size_t sites=attempts-before;CHECK(sites && sites<=UINT32_MAX);
    edges_capture_equal(staged,final);
    xr_compile_resources_free(staged->capture.facts);staged->capture=(EffectCaptureProof){0};
    CHECK(pc_stats(work).live_bytes==baseline && live_bytes==physical && live==blocks);
    for(size_t cut=0;cut<sites;++cut) {
        fail_at=attempts+cut;injected=false;
        XrXirStatus status=effect_capture_build(work,staged);
        fail_at=SIZE_MAX;CHECK(injected && status==XR_XIR_OUT_OF_MEMORY);
        xr_compile_resources_free(staged->capture.facts);staged->capture=(EffectCaptureProof){0};
        CHECK(pc_stats(work).live_bytes==baseline && live_bytes==physical && live==blocks);
        CHECK(effect_capture_build(work,staged)==XR_XIR_OK);
        edges_capture_equal(staged,final);
        xr_compile_resources_free(staged->capture.facts);staged->capture=(EffectCaptureProof){0};
        CHECK(pc_stats(work).live_bytes==baseline && live_bytes==physical && live==blocks);
    }
    fprintf(stderr,"actual global capture builder all%zu allocation faults and same-owner retry passed (focused only)\n",sites);
    xr_compile_resources_free(staged);
    CHECK(pc_stats(work).live_bytes==outer_live && live_bytes==outer_physical && live==outer_blocks);
}
static uint32_t edges_source_once(uint32_t cut,unsigned mode) {
    XrXirTypeNode type[2]={{.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE,.flags=edges_mask}};
    XrXirTypes types={.nodes=type,.count=edges_opaque?2u:1u};
    XrXirType scalar=XR_XIR_I64,callback=(XrXirType)256,receiver=(XrXirType)(edges_opaque?257:256);
    XrXirInstruction ret={.op=XR_XIR_RETURN};uint32_t operands[2]={0,0},actuals[2]={0,edges_opaque?2u:1u};
    XrXirInstruction ops[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={1,1},.immediate=2},
        {.op=XR_XIR_RETURN,.args={edges_no_return?0u:1u}}};
    XrXirInstruction caller[5]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=edges_opaque?XR_XIR_FUNCTION_WEAKEN:XR_XIR_CALL,.type=edges_opaque?(XrXirType)257:XR_XIR_UNIT,
            .args={1,edges_opaque?0u:1u},.immediate=edges_opaque?0:3},
        {.op=edges_opaque?XR_XIR_CALL:XR_XIR_RETURN,.args={edges_opaque?1u:0u,edges_opaque?1u:0u},
            .immediate=edges_opaque?3:0},{.op=XR_XIR_RETURN}};
    uint32_t loop_operands[4]={0,0,1,2};
    XrXirInstruction loop[13]={{.op=XR_XIR_JUMP,.targets={1}},
        {.op=XR_XIR_PHI,.type=(XrXirType)256,.args={0,4}},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=0},
        {.op=XR_XIR_BRANCH,.args={3},.targets={1,2}},{.op=XR_XIR_RETURN}};
    XrXirBlock blocks[8]={{.count=1},{.count=4},{.count=1},{.count=1},{.count=edges_opaque?5u:4u},
        {.first=0,.count=1},{.first=1,.count=3},{.first=4,.count=1}};
    if(edges_large) {
        for(uint32_t i=0;i<8;++i) loop[4+i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={i?4+i:2}};
        loop[12]=(XrXirInstruction){.op=XR_XIR_RETURN};blocks[7].count=9;
    }
    XrXirFunction functions[6]={
        {.name="init",.name_length=4,.instructions=&ret,.instruction_count=1,.blocks=&blocks[0],.block_count=1},
        {.name="factory",.name_length=7,.result=edges_no_return?XR_XIR_UNIT:(XrXirType)256,.instructions=ops,.instruction_count=4,
            .blocks=&blocks[1],.block_count=1,.operands=operands,.operand_count=2},
        {.name="scalar",.name_length=6,.parameters=&scalar,.parameter_count=1,.instructions=&ret,
            .instruction_count=1,.blocks=&blocks[2],.block_count=1},
        {.name="callback",.name_length=8,.parameters=&receiver,.parameter_count=1,.instructions=&ret,
            .instruction_count=1,.blocks=&blocks[3],.block_count=1},
        {.name="caller",.name_length=6,.instructions=caller,.instruction_count=edges_opaque?5u:4u,
            .blocks=&blocks[4],.block_count=1,.operands=actuals,.operand_count=2},
        {.name="cycle",.name_length=5,.parameters=&callback,.parameter_count=1,.instructions=loop,
            .instruction_count=edges_large?13u:5u,.blocks=&blocks[5],.block_count=3,.operands=loop_operands,.operand_count=4}};
    const XrXirSourceModule source={"edges",5,NULL,0,0};const XrXirFunctionIdentity identities[6]={0};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirEffectParameter plain={0},variable={.kind=XR_XIR_EFFECT_PARAMETER_VARIABLE},
        cycle_parameter={.kind=XR_XIR_EFFECT_PARAMETER_VARIABLE,.uses=XR_XIR_EFFECT_USE_COPY};
    XrXirFunctionEffectContract contracts[6]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0},
        {.parameter_count=1,.parameters=&plain},{.parameter_count=1,.parameters=&variable},{0},{.parameter_count=1,.parameters=&cycle_parameter}};
    const XrXirProvenance provenance={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=6};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_LIBRARY,.functions=functions,
        .function_count=6,.types=&types,.declarations=&declarations,.provenance=&provenance};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"edge admission%u f%u i%u reason%u\n",status,
        diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"actual effects analysis status%u work%llu large%u\n",status,(unsigned long long)pc_stats(&context).work,(unsigned)edges_large);
    CHECK(status==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;
    if(edges_faults) edges_capture_faults(&context,final);
    CHECK(final->site_count==3 && final->binding_count==3 && final->edge_count==4 && final->count==7);
    edges_direct=edges_reference=UINT32_MAX;
    for(uint32_t e=0;e<final->edge_count;++e) {
        if(final->edges[e].caller!=final->roots[4]) continue;
        if(final->edges[e].instruction==(edges_opaque?3u:2u)) edges_direct=e;
        if(final->edges[e].instruction==1) edges_reference=e;
    }
    CHECK(edges_direct<final->edge_count && edges_reference<final->edge_count);
    CHECK(final->edges[edges_direct].target!=final->roots[3]);
    const EffectInvocationNode *bound=&final->nodes[final->edges[edges_direct].target];
    CHECK(bound->body==3 && bound->root==0 && bound->input[0]==(edges_opaque?(8u|((edges_mask&XR_XIR_CALLABLE_ROOT_REQUIRED)?16u:0u)|
        ((edges_mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)?32u:0u)):4u) &&
        final->nodes[final->roots[3]].input[0]==(edges_opaque?8u:16u));
    uint64_t saved_binding=bound->input[0];
    for(uint32_t p=0;p<final->capture.count;++p)
        CHECK(final->capture.facts[p].node!=final->roots[5] || final->capture.facts[p].word ||
            final->capture.facts[p].bit>=final->site_count);
    XrXirStatus allocated_status=XR_XIR_OK;
    CHECK(final->capture.count && final->capture.count<256);
    EffectCaptureFact *residual=xir_compile_calloc(&context,256,sizeof(*residual),&allocated_status);
    CHECK(residual && allocated_status==XR_XIR_OK);
    memcpy(residual,final->capture.facts,final->capture.count*sizeof(*residual));
    xr_compile_resources_free(final->capture.facts);final->capture.facts=residual;final->capture.capacity=256;
    EffectCaptureFact saved_residual[256];
    memcpy(saved_residual,residual,final->capture.count*sizeof(*residual));
    EffectInvocationFinal saved=*final;EffectInvocationEdge saved_edges[32];EffectInvocationNode saved_nodes[32];
    CHECK(final->edge_count<=32 && final->count<=32);
    memcpy(saved_edges,final->edges,final->edge_count*sizeof(*final->edges));
    memcpy(saved_nodes,final->nodes,final->count*sizeof(*final->nodes));
    uint64_t saved_inputs[32]={0};
    for(uint32_t n=0;n<final->count;++n) if(final->nodes[n].input) saved_inputs[n]=final->nodes[n].input[0];
    edges_final=mode?final:NULL;edges_cut=cut;edges_frees=0;edges_attacks=0;edges_mode=mode;
    CHECK(!pc_free_hook);pc_free_hook=edges_free;
    status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;edges_final=NULL;uint32_t frees=edges_frees;CHECK(!mode || edges_attacks==1);
    fprintf(stderr,"actual Source edges mode%u cut%u frees%u status%u expected%u no_return%u opaque%u\n",mode,cut,frees,status,
        mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK,(unsigned)edges_no_return,(unsigned)edges_opaque);
    *final=saved;memcpy(final->edges,saved_edges,final->edge_count*sizeof(*final->edges));
    memcpy(final->capture.facts,saved_residual,final->capture.count*sizeof(*final->capture.facts));
    memcpy(final->nodes,saved_nodes,final->count*sizeof(*final->nodes));
    final->nodes[saved_edges[edges_direct].target].input[0]=saved_binding;
    for(uint32_t n=0;n<final->count;++n) if(final->nodes[n].input) final->nodes[n].input[0]=saved_inputs[n];
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(ops,0xDC,sizeof(ops));memset(caller,0xDC,sizeof(caller));
    memset(functions,0xDC,sizeof(functions));memset(loop,0xDC,sizeof(loop));memset(type,0xDC,sizeof(type));
    memset(operands,0xDC,sizeof(operands));memset(actuals,0xDC,sizeof(actuals));memset(loop_operands,0xDC,sizeof(loop_operands));
    memset(contracts,0xDC,sizeof(contracts));size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=edges_forbid;pc_free_hook=edges_forbid;
    CHECK(effect_final_inventories_match(&context,effects->invocations,final->outcomes)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && live_bytes==physical);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    CHECK(status==(mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));return frees;
}
#include "xir_capture_complete_cases.h"
#include "xir_capture_callable_cases.h"
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"captured")) { complete_captured_case(0);complete_captured_case(1);return 0; }
    if(argc==2 && !strcmp(argv[1],"captured-nested")) { complete_captured_case(2);complete_captured_case(3);return 0; }
    if(argc==2 && !strcmp(argv[1],"complete")) { complete_cases();return 0; }
    CHECK(argc==2 || argc==3);
    if(argc==3) CHECK(!strcmp(argv[2],"no-return") || !strcmp(argv[2],"proof-fi") || !strcmp(argv[2],"proof-fi-large") || !strcmp(argv[2],"large"));
    edges_no_return=argc==3 && !strcmp(argv[2],"no-return");edges_large=argc==3 && (!strcmp(argv[2],"proof-fi-large") || !strcmp(argv[2],"large"));edges_faults=argc==3 && (!strcmp(argv[2],"proof-fi-large") || !strcmp(argv[2],"proof-fi"));unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<=60);edges_opaque=mode>=31;
    edges_mask=(mode==48 || mode==51)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:
        (mode==49 || mode==50)?(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED):XR_XIR_CALLABLE_ROOT_REQUIRED;
    uint32_t frees=edges_source_once(UINT32_MAX,0);CHECK(frees);
    if(mode) CHECK(edges_source_once(frees,mode)==frees);return 0;
}
