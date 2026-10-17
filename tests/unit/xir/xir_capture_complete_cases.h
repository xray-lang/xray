/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_capture_complete_cases.h - Returned callable alternatives and evidence
 */
static void complete_verify(const XrXirCompileContext *work,const EffectInvocationFinal *final,XrXirStatus expected) {
    size_t before=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=edges_forbid;pc_free_hook=edges_forbid;
    XrXirStatus status=effect_capture_match(work,final);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    CHECK(status==expected && attempts==before && live_bytes==physical);
}

/* Literal SSA coordinates are specific to these ten fixture recipes and do
 * not call a production flow solver or derive their expectation from facts. */
static void complete_literal_coordinates(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,uint32_t mode) {
    static const uint32_t factory[10]={1u,3u,1u,11u,1u,20u,20u,84u,20u,1u};
    bool known=mode==0 || mode==1 || mode==3 || mode==6 || mode==7;
    EffectCaptureRead read={.work=work,.final=final,.body=effect_body_owned_request(work,final->material)};
    for(uint32_t n=0;n<final->count;++n) {
        EffectCaptureNode node={0};CHECK(effect_capture_node(&read,n,&node)==XR_XIR_OK);
        uint32_t body=final->nodes[n].body;
        for(uint32_t value=0;value<=node.values+node.head.parameter_count;++value) {
            bool expected=false;
            if(body==0 && !value) expected=known;
            if(body==1) expected=value<node.values?!!(factory[mode]&(UINT32_C(1)<<value)):value==node.values && known;
            if(body==3 && (value==0 || value==node.values+1)) {
                CHECK(final->site_count<64 && final->nodes[n].input);
                uint64_t sites=(UINT64_C(1)<<final->site_count)-1;
                expected=n!=final->roots[3] && known && !!(final->nodes[n].input[0]&sites);
            }
            bool actual=false;CHECK(effect_final_callable_complete(work,final,n,value,&actual)==XR_XIR_OK);
            if(actual!=expected) fprintf(stderr,"complete literal mode%u node%u body%u value%u expected%u actual%u root%u input%llu\n",
                mode,n,body,value,expected,actual,final->roots[body],
                (unsigned long long)(final->nodes[n].input?final->nodes[n].input[0]:0));
            CHECK(actual==expected);
        }
    }
}

static EffectBodyFunction *complete_growth_head;
static EffectCaptureFact *complete_growth_facts;
static uint32_t complete_growth_callbacks;
static bool complete_growth_attack;
static void complete_growth_free(void *memory,size_t bytes) {
    uintptr_t first=(uintptr_t)memory,query=(uintptr_t)complete_growth_facts;
    if(query<first || query-first>=bytes) return;
    ++complete_growth_callbacks;pc_free_hook=NULL;
    if(complete_growth_attack) complete_growth_head->runs=(void *)(uintptr_t)1;
}

/* Reconstruct COMPLETE facts in a full capacity relation. The first growth's
 * real release callback replaces an actual Input run pointer. A stack view
 * opened before that callback must not authorize any later payload read. */
static void complete_growth_boundary(const XrXirCompileContext *work,EffectInvocationFinal *final) {
    EffectCaptureProof saved=final->capture;uint32_t count=0;
    for(uint32_t p=0;p<saved.count;++p)
        if(saved.facts[p].word!=EFFECT_CAPTURE_FLAGS || saved.facts[p].bit<EFFECT_CAPTURE_COMPLETE) ++count;
    CHECK(count && count<saved.count);
    complete_growth_head=&final->material->functions[final->nodes[final->roots[1]].body];
    EffectBodyRun *runs=complete_growth_head->runs;CHECK(runs);
    for(uint32_t attack=0;attack<2;++attack) {
        XrXirStatus status=XR_XIR_OK;uint32_t at=0;
        EffectCaptureFact *facts=xir_compile_alloc(work,(size_t)count*sizeof(*facts),&status);CHECK(facts);
        for(uint32_t p=0;p<saved.count;++p)
            if(saved.facts[p].word!=EFFECT_CAPTURE_FLAGS || saved.facts[p].bit<EFFECT_CAPTURE_COMPLETE) facts[at++]=saved.facts[p];
        final->capture=(EffectCaptureProof){.facts=facts,.count=count,.capacity=count};
        EffectCaptureRead read={.work=work,.final=final};
        CHECK(effect_capture_frame(&read)==XR_XIR_OK);
        read.builder=&final->capture;complete_growth_facts=facts;
        complete_growth_callbacks=0;complete_growth_attack=attack!=0;pc_free_hook=complete_growth_free;
        do { read.changed=false;status=effect_capture_complete_clauses(&read); } while(status==XR_XIR_OK && read.changed);
        pc_free_hook=NULL;
        CHECK(complete_growth_callbacks==1 && !read.body.current && !read.node_view);
        CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        complete_growth_head->runs=runs;
        if(!attack) complete_verify(work,final,XR_XIR_OK);
        xr_compile_resources_free(final->capture.facts);final->capture=saved;
        complete_verify(work,final,XR_XIR_OK);
    }
    complete_growth_head=NULL;complete_growth_facts=NULL;
}

#include "xir_capture_final_view_cases.h"

static void complete_case(uint32_t mode) {
    CHECK(mode<10);XrXirType callback=256;
    XrXirTypeNode type={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypes types={&type,1,NULL,NULL};uint32_t argument=0;
    XrXirInstruction entry[3]={{.op=XR_XIR_CALL,.type=callback,.immediate=1},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=3,.args={0,1}},{.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction factory[10]={{.op=XR_XIR_FUNCTION_REF,.type=callback,.immediate=2},{.op=XR_XIR_RETURN}};
    XrXirInstruction pure[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},{.op=XR_XIR_RETURN}};
    XrXirInstruction apply[2]={{.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirBlock blocks[6]={{.count=3},{.count=2},{.count=2},{.count=2},{.count=1},{0}};
    if(mode==1 || mode==2) {
        factory[1]=(XrXirInstruction){.op=mode==1?XR_XIR_COPY:XR_XIR_FUNCTION_WEAKEN,.type=callback};
        factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};blocks[1].count=3;
    } else if(mode==3 || mode==4 || mode==9) {
        factory[1]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=callback};
        uint32_t write=2;
        if(mode==4) { factory[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=callback};write=3; }
        factory[write]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={1,mode==4?2u:0u}};
        factory[write+1]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=callback,.args={1,0}};
        factory[write+2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={write+1,0}};blocks[1].count=write+3;
        if(mode==9) {
            factory[2]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=callback,.args={1,0}};
            factory[3]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={1,2}};
            factory[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        }
    } else if(mode==5 || mode==6) {
        factory[2]=factory[4]=factory[0];factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        factory[1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.targets={1,2}};uint32_t exit=5;
        if(mode==5) { factory[5]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=callback,.args={4,0}};exit=6; }
        factory[exit]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={mode==5?5u:4u,0}};
        blocks[5]=(XrXirBlock){.first=4,.count=exit-3};
    }
    XrXirBlock factory_blocks[4]={blocks[1],{.first=2,.count=2},blocks[5],{0}};
    uint32_t phi[4]={1,2,2,mode==8?5u:4u};
    if(mode==7 || mode==8) {
        factory[2]=factory[4]=factory[0];
        factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        factory[1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.targets={1,2}};
        factory[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};uint32_t jump=5;
        if(mode==8) { factory[5]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=callback,.args={4,0}};jump=6; }
        factory[jump]=factory[3];factory[jump+1]=(XrXirInstruction){.op=XR_XIR_PHI,.type=callback,.args={0,4}};
        factory[jump+2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={jump+1,0}};
        factory_blocks[2]=(XrXirBlock){.first=4,.count=jump-3};
        factory_blocks[3]=(XrXirBlock){.first=jump+1,.count=2};
    }
    XrXirFunction functions[5]={
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.instructions=entry,.instruction_count=3,
            .blocks=&blocks[0],.block_count=1,.operands=&argument,.operand_count=1},
        {.name="factory",.name_length=7,.result=callback,.instructions=factory,
            .instruction_count=mode>=5 && mode<9?factory_blocks[mode>=7?3:2].first+factory_blocks[mode>=7?3:2].count:blocks[1].count,
            .blocks=factory_blocks,.block_count=mode>=7 && mode<9?4u:mode>=5 && mode<9?3u:1u,
            .operands=mode>=7 && mode<9?phi:NULL,.operand_count=mode>=7 && mode<9?4u:0u},
        {.name="pure",.name_length=4,.result=XR_XIR_I64,.instructions=pure,.instruction_count=2,.blocks=&blocks[2],.block_count=1},
        {.name="apply",.name_length=5,.result=XR_XIR_I64,.parameters=&callback,.parameter_count=1,
            .instructions=apply,.instruction_count=2,.blocks=&blocks[3],.block_count=1},
        {.name="init",.name_length=4,.instructions=&init,.instruction_count=1,.blocks=&blocks[4],.block_count=1}};
    const XrXirSourceModule source={.name="complete",.name_length=8,.initializer=4};
    XrXirFunctionIdentity identities[5]={0};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.types=&types,
        .functions=functions,.function_count=5,.declarations=&declarations};
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus admission=xir_fixture_check(&work,&module,&checked,&diagnostic);
    if(admission!=XR_XIR_OK) fprintf(stderr,"complete mode%u admission%u f%u b%u i%u reason%u\n",mode,
        admission,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(admission==XR_XIR_OK);
    XrXirStatus status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"complete mode%u analysis%u work%llu\n",mode,status,(unsigned long long)pc_stats(&work).work);
    CHECK(status==XR_XIR_OK);EffectInvocationFinal *final=effects->invocations->final;
    CHECK(xr_xir_effects_error_unknown(effects,0)==(mode==2 || mode==4 || mode==5 || mode==8 || mode==9));
    CHECK(xr_xir_effects_error_unknown(effects,3));complete_literal_coordinates(&work,final,mode);
    if(!mode) complete_growth_boundary(&work,final);
    xr_xir_compile_artifact_free(checked);memset(factory,0xCC,sizeof(factory));memset(functions,0xCC,sizeof(functions));
    if(!mode) complete_final_view_boundary(&work,effects->invocations);
    complete_verify(&work,final,XR_XIR_OK);
    CHECK(final->capture.count<512 && final->capture.count+2<=final->capture.capacity);
    EffectCaptureFact saved[512];uint32_t count=final->capture.count;
    memcpy(saved,final->capture.facts,count*sizeof(*saved));
    for(uint32_t p=0;p<count;++p) if(saved[p].word==EFFECT_CAPTURE_FLAGS && saved[p].bit==EFFECT_CAPTURE_COMPLETE) {
        final->capture.facts[p].rank=0;
        if(saved[p].rank) complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);
        memcpy(final->capture.facts,saved,count*sizeof(*saved));
    }
    for(uint32_t p=0;p<final->capture.count;) {
        EffectCaptureFact *fact=&final->capture.facts[p];
        if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_COMPLETE) {
            --final->capture.count;memmove(fact,fact+1,(size_t)(final->capture.count-p)*sizeof(*fact));
        } else ++p;
    }
    complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);
    final->capture.count=count;memcpy(final->capture.facts,saved,count*sizeof(*saved));
    edges_capture_fake(final,(EffectCaptureFact){final->roots[3],0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});
    complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);
    final->capture.count=count;memcpy(final->capture.facts,saved,count*sizeof(*saved));
    if(mode==9) {
        edges_capture_fake(final,(EffectCaptureFact){final->roots[1],1,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,1});
        edges_capture_fake(final,(EffectCaptureFact){final->roots[1],2,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});
        complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);
        final->capture.count=count;memcpy(final->capture.facts,saved,count*sizeof(*saved));
    }
    complete_verify(&work,final,XR_XIR_OK);
    xr_xir_compile_effects_free(effects);pc_release(&work,baseline);
    printf("complete mode%u: literal Error, literal SSA coordinates, dead producer, erasure and forged ranks\n",mode);
}

static void complete_cases(void) { for(uint32_t mode=0;mode<10;++mode) complete_case(mode); }
