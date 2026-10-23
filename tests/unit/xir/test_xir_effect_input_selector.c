/* An empty opcode result must still reject impossible owned storage. */
#define main selector_original_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
#include "xir_root_conditional_raw_fixture.h"
static void selector_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static void selector_scalar_domain(void) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE}};
    XrXirTypes types={.nodes=nodes,.count=2};XrXirType parameter=XR_XIR_I64;
    XrXirInstruction done={.op=XR_XIR_RETURN},ops[3]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},{.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock blocks[2]={{.count=1},{.count=3}};
    XrXirFunction functions[2]={{.name="init",.name_length=4,.error_type=XR_XIR_NEVER,
        .instructions=&done,.instruction_count=1,.blocks=blocks,.block_count=1},
        {.name="scalar",.name_length=6,.error_type=XR_XIR_NEVER,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .instructions=ops,.instruction_count=3,.blocks=&blocks[1],.block_count=1}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirSourceModule identity={.name="scalar",.name_length=6};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule module={.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=2,
        .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_infer_verified(&context,&module,&effects)==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;EffectBodyMaterial *material=final->material;
    EffectCaptureRead read={.work=&context,.final=final,.body=effect_body_owned_request(&context,material)};
    memset(functions,0xDC,sizeof(functions));memset(ops,0xDC,sizeof(ops));memset(nodes,0xDC,sizeof(nodes));
    size_t allocated=attempts;uint64_t physical=live_bytes;bool scalar=false;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && scalar);
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK);
    EffectBodyFunction *head=&material->functions[1];XrXirType *formal=(XrXirType *)head->parameters;
    XrXirType saved_type=formal[0];formal[0]=(XrXirType)256;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);formal[0]=(XrXirType)4;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);formal[0]=saved_type;
    XrXirType result=head->result;head->result=(XrXirType)257;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);head->result=result;
    EffectBodyRun *first=&head->runs[0];CHECK(first->count==2);
    uint32_t mask=first->mask,opcode_delta=first->delta[0],type_delta=first->delta[1];
    first->mask|=UINT32_C(2);first->delta[1]=256u-(uint32_t)first->base.type;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);
    first->mask=mask;first->delta[1]=type_delta;
    first->mask|=UINT32_C(1);first->delta[0]=(uint32_t)XR_XIR_FUNCTION_REF-(uint32_t)first->base.op;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);
    first->mask=mask;first->delta[0]=opcode_delta;
    XrXirOp saved_opcode=first->base.op;uint32_t argument_delta=first->delta[2];
    first->base.op=XR_XIR_LOCAL_WRITE;first->mask|=UINT32_C(4);first->delta[2]=UINT32_MAX;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_BAD_STRUCTURE && !scalar);
    first->base.op=saved_opcode;first->mask=mask;first->delta[2]=argument_delta;
    first->base.op=XR_XIR_OP_COUNT;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_BAD_STRUCTURE && !scalar);first->base.op=saved_opcode;
    EffectInvocationNode *root=&final->nodes[final->roots[1]];CHECK(root->input && !root->input[0]);
    root->input[0]=1;CHECK(effect_capture_match(&context,final)==XR_XIR_BAD_STRUCTURE);root->input[0]=0;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && scalar);
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK && attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    puts("primitive capture domain: unused Cell/callable types, live formal and affine type/opcode changes, invalid opcode, false scalar input, dead producer and physical0");
}
typedef struct SelectorRawSource {
    ConditionalRawFixture raw;
    XrXirInstruction forward[4],entry[2];
    XrXirFunction functions[5];XrXirGeneric generics[5];
    XrXirFunctionIdentity identities[5];XrXirBlock blocks[5];
} SelectorRawSource;
static void selector_raw_source_fixture(SelectorRawSource *f) {
    memset(f,0,sizeof(*f));conditional_raw_fixture(&f->raw,2);
    for(uint32_t n=0;n<6;++n) {
        if(f->raw.nodes[n].kind==XR_XIR_TYPE_CALLABLE) f->raw.nodes[n].element=XR_XIR_NEVER;
    }
    for(uint32_t n=0;n<4;++n) f->raw.functions[n].error_type=XR_XIR_NEVER;
    f->forward[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    f->forward[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    f->forward[2]=f->raw.forward_ops[0];f->forward[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};
    f->raw.functions[1].instructions=f->forward;f->raw.functions[1].instruction_count=4;
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    memcpy(f->functions,f->raw.functions,sizeof(f->raw.functions));
    memcpy(f->generics,f->raw.generics,sizeof(f->raw.generics));
    memcpy(f->identities,f->raw.identities,sizeof(f->raw.identities));
    f->functions[4]=(XrXirFunction){.name="entry",.name_length=5,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=2};
    for(uint32_t n=0;n<5;++n) {
        f->blocks[n]=(XrXirBlock){.count=f->functions[n].instruction_count};
        f->functions[n].blocks=&f->blocks[n];f->functions[n].block_count=1;
    }
    f->raw.declarations.functions=f->identities;f->raw.declarations.entry_function=4;
    f->raw.module.functions=f->functions;f->raw.module.function_count=5;f->raw.module.generics=f->generics;
}
static void selector_raw_source_replay(bool resources) {
    size_t sites=0;uint64_t peak=0,maximum=0;
    for(size_t pass=0;pass<=(resources?sites:0);++pass) {
        SelectorRawSource fixture;selector_raw_source_fixture(&fixture);
        XrCompileResourceLimits caps=pc_caps();caps.work=4000000;
        XrXirCompileContext context=pc_owner(caps);context.limits.functions=32;
        context.limits.parameters=16;context.limits.instructions=128;
        uint64_t baseline=pc_stats(&context).live_bytes;
        XrXirArtifact *checked=NULL;
        CHECK(xir_fixture_check(&context,&fixture.raw.module,&checked,NULL)==XR_XIR_OK && checked);
        memset(&fixture,0xDC,sizeof(fixture));
        uint64_t held=pc_stats(&context).live_bytes;XrCompileResources *identity=context.resources;
        attempts=0;fail_at=pass?pass-1:SIZE_MAX;injected=false;
        XrXirEffects *effects=NULL;
        XrXirStatus status=xir_artifact_verify_raw_source_effects(checked,&effects,NULL);
        if(!pass) { CHECK(status==XR_XIR_OK && effects);sites=attempts; }
        else {
            if(status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"raw replay allocation%zu status%u work%llu\n",
                pass-1,status,(unsigned long long)pc_stats(&context).work);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && !effects && pc_stats(&context).live_bytes==held);
            fail_at=SIZE_MAX;
            CHECK(xir_artifact_verify_raw_source_effects(checked,&effects,NULL)==XR_XIR_OK && effects);
        }
        fail_at=SIZE_MAX;uint64_t retained=pc_stats(&context).live_bytes;
        const XrXirModule *module=xr_xir_compile_artifact_module(checked);
        if(!pass) {
            XrCompileResourceStats before=pc_stats(&context);
            pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
            CHECK(effect_capture_match(&context,effects->invocations->final)==XR_XIR_OK);
            pc_allocate_hook=NULL;pc_free_hook=NULL;
            XrCompileResourceStats after=pc_stats(&context);
            CHECK(after.work>before.work && after.allocated_bytes==before.allocated_bytes &&
                after.live_bytes==retained && after.allocation_count==before.allocation_count && context.resources==identity);
            printf("capture current support: original5-function input facts%u work%llu allocated%llu live%llu peak%llu no callbacks\n",
                effects->invocations->final->capture.count,(unsigned long long)(after.work-before.work),
                (unsigned long long)after.allocated_bytes,(unsigned long long)after.live_bytes,(unsigned long long)after.peak_bytes);
        }
        for(uint32_t read=0;read<2;++read) {
            uint64_t before=pc_stats(&context).work;
            CHECK(xir_compile_verify_raw_source_effects(&context,module,checked->construction,effects,NULL)==XR_XIR_OK);
            CHECK(pc_stats(&context).work>before && pc_stats(&context).live_bytes==retained && context.resources==identity);
        }
        XrCompileResourceStats stats=pc_stats(&context);
        if(stats.peak_bytes>peak) peak=stats.peak_bytes;
        if(stats.work>maximum) maximum=stats.work;
        xr_xir_compile_effects_free(effects);CHECK(pc_stats(&context).live_bytes==held);
        xr_xir_compile_artifact_free(checked);pc_release(&context,baseline);
    }
    printf("raw source replay: original5-function captured generic input, 4M owner, dead producer, twice current proof, FI%zu same-owner retries, maxwork%llu peak%llu physical0\n",
        resources?sites:0,(unsigned long long)maximum,(unsigned long long)peak);
}
static void selector_capture_support(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectCaptureFact facts[64];
    for(uint32_t p=0;p<64;++p) facts[p]=(EffectCaptureFact){2,3,p/32,p%32,p+3};
    EffectCaptureRead read={.work=&context,.selected=facts,.selected_count=64};
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(uint32_t p=63;p>0;--p) CHECK(effect_capture_emit(&read,facts[p])==XR_XIR_OK && !read.supported);
    CHECK(read.selected_mask==UINT64_MAX-1);
    EffectCaptureFact cause=facts[0];++cause.rank;
    CHECK(effect_capture_emit(&read,cause)==XR_XIR_OK && !read.supported && read.selected_mask==UINT64_MAX-1);
    cause=facts[0];++cause.node;
    CHECK(effect_capture_emit(&read,cause)==XR_XIR_OK && !read.supported && read.selected_mask==UINT64_MAX-1);
    CHECK(effect_capture_emit(&read,facts[0])==XR_XIR_OK && read.supported && read.selected_mask==UINT64_MAX);
    EffectCaptureFact flags[2]={{2,3,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,3},
        {2,3,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,7}};
    read.selected=flags;read.selected_count=2;read.selected_mask=0;read.supported=false;
    CHECK(effect_capture_emit(&read,flags[1])==XR_XIR_OK && !read.supported && read.selected_mask==2);
    CHECK(effect_capture_emit(&read,flags[0])==XR_XIR_OK && read.supported && read.selected_mask==3);
    CHECK(attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;pc_release(&context,baseline);
    puts("capture support: all64 obligations across words, individual ranks, fixed/raw flags, no callbacks and physical0");
}
/* A reference followed by k copies has exactly k+1 finite origins and
 * RAW flags and Complete witnesses, with literal distance ranks 0 through k. These
 * cases straddle the constructor's bounded ordinal table without changing
 * the source relation or the independent exhaustive receiving obligation. */
static void selector_complete_chain(void) {
    for(uint32_t copies=126;copies<=128;++copies) {
        XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
            .result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_NONE};
        XrXirTypes types={.nodes=&callable,.count=1};
        XrXirInstruction done={.op=XR_XIR_RETURN},ops[130]={0};
        ops[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=1};
        for(uint32_t i=1;i<=copies;++i) ops[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={i-1}};
        ops[copies+1]=done;
        XrXirBlock blocks[3]={{.count=1},{.count=1},{.count=copies+2}};
        XrXirFunction functions[3]={{.name="init",.name_length=4,.error_type=XR_XIR_NEVER,
            .instructions=&done,.instruction_count=1,.blocks=&blocks[0],.block_count=1},
            {.name="leaf",.name_length=4,.error_type=XR_XIR_NEVER,
                .instructions=&done,.instruction_count=1,.blocks=&blocks[1],.block_count=1},
            {.name="chain",.name_length=5,.error_type=XR_XIR_NEVER,
                .instructions=ops,.instruction_count=copies+2,.blocks=&blocks[2],.block_count=1}};
        XrXirFunctionIdentity identities[3]={{0}};
        XrXirSourceModule identity={.name="chain",.name_length=5};
        XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,
            .root_module=UINT32_MAX,.entry_function=2};
        XrXirModule module={.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=3,
            .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_infer_verified(&context,&module,&effects)==XR_XIR_OK && effects);
        EffectInvocationFinal *final=effects->invocations->final;
        CHECK(final->site_count==1 && !final->binding_count && final->sites[0].function==2 &&
            !final->sites[0].instruction && final->sites[0].target==1 && !final->sites[0].captures);
        CHECK(final->capture.count==3*(copies+1));
        uint32_t origins=0,raw=0,complete=0,node=final->roots[2];
        EffectCaptureFact *last=NULL;
        for(uint32_t f=0;f<final->capture.count;++f) {
            EffectCaptureFact *fact=&final->capture.facts[f];
            CHECK(fact->node==node && fact->value<=copies && fact->rank==fact->value);
            if(!fact->word && !fact->bit) ++origins;
            else if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_RAW) ++raw;
            else { CHECK(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_COMPLETE);++complete;last=fact; }
        }
        CHECK(origins==copies+1 && raw==copies+1 && complete==copies+1 && last);
        memset(ops,0xDC,sizeof(ops));memset(functions,0xDC,sizeof(functions));
        size_t allocated=attempts;uint64_t physical=live_bytes;
        pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
        CHECK(effect_capture_match(&context,final)==XR_XIR_OK);
        uint64_t rank=last->rank;last->rank=rank+1;
        CHECK(effect_capture_match(&context,final)==XR_XIR_BAD_STRUCTURE);last->rank=rank;
        CHECK(effect_capture_match(&context,final)==XR_XIR_OK && attempts==allocated && live_bytes==physical);
        pc_allocate_hook=NULL;pc_free_hook=NULL;
        xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    }
    puts("Complete constructor:127/128/129 literal copy-chain witnesses, exact identities/ranks, exhaustive receiving and physical0 PASS");
}
typedef struct SelectorCurrentPoll { bool requested;uint32_t calls; } SelectorCurrentPoll;
static bool selector_current_poll(const void *context) {
    SelectorCurrentPoll *poll=(SelectorCurrentPoll *)context;++poll->calls;return poll->requested;
}
static void selector_current_handoff(void) {
    SelectorRawSource fixture;selector_raw_source_fixture(&fixture);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    context.limits.functions=32;context.limits.parameters=16;context.limits.instructions=128;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.raw.module,&checked,NULL)==XR_XIR_OK && checked);
    memset(&fixture,0xDC,sizeof(fixture));
    CHECK(xir_artifact_verify_raw_source_effects(checked,&effects,NULL)==XR_XIR_OK && effects);
    EffectInvocationFinal *final=effects->invocations->final;
    SelectorCurrentPoll poll={0};
    CHECK(xr_compile_resources_begin_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    {
        EffectBodyCurrentRead current={0};CHECK(effect_body_current_open(&context,final->material,&current)==XR_XIR_OK);
        size_t allocated=attempts;uint64_t physical=live_bytes;uint32_t calls=poll.calls;
        pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
        CHECK(effect_capture_current_match(&context,final,&current)==XR_XIR_OK && poll.calls>calls);
        poll.requested=true;
        CHECK(effect_capture_current_match(&context,final,&current)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_end_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
        CHECK(attempts==allocated && live_bytes==physical);
        current=(EffectBodyCurrentRead){0};
        pc_allocate_hook=NULL;pc_free_hook=NULL;
    }
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&context,baseline);
    puts("current handoff: owned parent scope, independent receiving, cancellation polling/latch/recovery, no allocator callbacks and physical0 PASS");
}

typedef struct SelectorHeadProjection {
    uint32_t calls,deny,function[2];
    bool dimensions[2];uint64_t fee[2];
} SelectorHeadProjection;
static SelectorHeadProjection selector_head_projection;
static XrXirStatus selector_projection_head(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *full,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    SelectorHeadProjection *trace=&selector_head_projection;uint32_t call=trace->calls++;
    CHECK(call<2 && !signature && (!!full+!!dimensions)==1);
    trace->function[call]=f;trace->dimensions[call]=dimensions!=NULL;
    if(trace->calls==trace->deny) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *work=request->read.owner;uint64_t before=pc_stats(work).work;
    XrXirStatus status=effect_body_owned_head(request,f,full,dimensions,signature);
    trace->fee[call]=pc_stats(work).work-before;return status;
}
static void selector_projection_storage(EffectBodyFunction *head,uint32_t mode) {
    switch(mode) {
    case 0:head->name_length=1;head->name=NULL;break;
    case 1:head->parameter_count=1;head->parameters=NULL;break;
    case 2:head->block_count=1;head->blocks=NULL;break;
    case 3:head->operand_count=1;head->operands=NULL;break;
    case 4:head->instruction_count=1;head->run_count=1;head->run_capacity=1;head->runs=NULL;break;
    default:head->run_count=head->run_capacity+1;break;
    }
}
/* Both projections retain the same whole-head storage predicate. The source
 * still supplies operands, while the target supplies only its formal count. */
static void selector_invocation_projection(void) {
    SelectorRawSource fixture;selector_raw_source_fixture(&fixture);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    context.limits.functions=32;context.limits.parameters=16;context.limits.instructions=128;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.raw.module,&checked,NULL)==XR_XIR_OK && checked);
    CHECK(xir_artifact_verify_raw_source_effects(checked,&effects,NULL)==XR_XIR_OK && effects);
    memset(&fixture,0xDC,sizeof(fixture));
    EffectInvocationFinal *final=effects->invocations->final;EffectBodyMaterial *material=final->material;
    uint32_t selected=UINT32_MAX;
    for(uint32_t e=0;e<final->edge_count;++e) {
        const EffectInvocationEdge *edge=&final->edges[e];
        if(final->nodes[edge->caller].body!=final->nodes[edge->target].body) {
            selected=e;break;
        }
    }
    CHECK(selected!=UINT32_MAX);
    const EffectInvocationEdge *edge=&final->edges[selected];
    uint32_t source=final->nodes[edge->caller].body,target=final->nodes[edge->target].body;
    EffectBodyRequest body=effect_body_owned_request(&context,material);body.head=selector_projection_head;
    XrXirCompileContext foreign=context;foreign.resources=NULL;XrCompileResourceLimits caps=pc_caps();
    CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(uint32_t deny=0;deny<=2;++deny) {
        selector_head_projection=(SelectorHeadProjection){.deny=deny};
        CHECK(effect_final_edge_payload(&context,&body,final,selected)==(deny?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(selector_head_projection.calls==(deny?deny:2));
        CHECK(selector_head_projection.function[0]==source && !selector_head_projection.dimensions[0]);
        if(deny!=1) CHECK(selector_head_projection.function[1]==target && selector_head_projection.dimensions[1]);
        if(!deny) CHECK(selector_head_projection.fee[0]==12+sizeof(EffectBodyFunctionHead) &&
            selector_head_projection.fee[1]==11+sizeof(EffectBodyFunctionDimensions));
    }
    for(uint32_t f=0;f<effect_body_count(material);++f) for(uint32_t mode=0;mode<6;++mode) {
        EffectBodyFunction *head=&material->functions[f];EffectBodyFunction saved=*head;
        selector_projection_storage(head,mode);
        CHECK(effect_final_inventory_sites(&context,material,final)==XR_XIR_BAD_STRUCTURE);
        if(f==target) {
            selector_head_projection=(SelectorHeadProjection){0};
            CHECK(effect_final_edge_payload(&context,&body,final,selected)==XR_XIR_BAD_STRUCTURE);
        }
        *head=saved;
        CHECK(effect_final_inventory_sites(&context,material,final)==XR_XIR_OK);
    }
    CHECK(effect_final_inventory_match(&foreign,material,final)==XR_XIR_BAD_STRUCTURE);
    SelectorCurrentPoll poll={.requested=true};
    CHECK(xr_compile_resources_begin_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    selector_head_projection=(SelectorHeadProjection){0};
    CHECK(effect_final_edge_payload(&context,&body,final,selected)==XR_XIR_BUDGET && poll.calls);
    CHECK(xr_compile_resources_end_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
    selector_head_projection=(SelectorHeadProjection){0};
    CHECK(effect_final_edge_payload(&context,&body,final,selected)==XR_XIR_OK);
    CHECK(effect_final_inventory_match(&context,material,final)==XR_XIR_OK);
    CHECK(attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;xr_compile_resources_release(foreign.resources);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&context,baseline);
    puts("invocation projections: source full/target dimensions, exact fees, both callback refusals, all head storage preconditions, foreign ledger, cancellation/recovery and physical0 PASS");
}


typedef struct SelectorSignatureTrace {
    uint32_t calls,deny,kind[4],function[4];uint64_t fee[4];
} SelectorSignatureTrace;
static SelectorSignatureTrace selector_signature_trace;
static XrXirStatus selector_signature_head(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *full,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    SelectorSignatureTrace *trace=&selector_signature_trace;uint32_t call=trace->calls++;
    CHECK(call<4 && (!!full+!!dimensions+!!signature)==1);
    trace->kind[call]=signature?3:dimensions?2:1;trace->function[call]=f;
    if(trace->calls==trace->deny) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *work=request->read.owner;uint64_t before=pc_stats(work).work;
    XrXirStatus status=effect_body_owned_head(request,f,full,dimensions,signature);
    trace->fee[call]=pc_stats(work).work-before;return status;
}
/* The literal REF captures a receiver and callback. Its target signature is
 * enough for identity typing; the source still provides the complete operands. */
static void selector_capture_signature(void) {
    SelectorRawSource fixture;selector_raw_source_fixture(&fixture);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    context.limits.functions=32;context.limits.parameters=16;context.limits.instructions=128;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.raw.module,&checked,NULL)==XR_XIR_OK && checked);
    CHECK(xir_artifact_verify_raw_source_effects(checked,&effects,NULL)==XR_XIR_OK && effects);
    memset(&fixture,0xDC,sizeof(fixture));
    EffectInvocationFinal *final=effects->invocations->final;EffectBodyMaterial *material=final->material;
    CHECK(final->site_count==1 && final->binding_count==2);
    const EffectInvocationSite *site=&final->sites[0];
    CHECK(site->function==0 && site->instruction==0 && site->target==2 && site->captures==2 && !site->binding);
    EffectCaptureRead read={.work=&context,.final=final,.body=effect_body_owned_request(&context,material)};
    EffectCaptureNode node={0};CHECK(effect_capture_node(&read,final->roots[0],&node)==XR_XIR_OK);
    read.body.head=selector_signature_head;
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(uint32_t deny=0;deny<=2;++deny) {
        selector_signature_trace=(SelectorSignatureTrace){.deny=deny};bool seeded=false;
        CHECK(effect_capture_reference_seed(&read,0,&seeded)==(deny?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(seeded==!deny && selector_signature_trace.calls==(deny?deny:2));
        CHECK(selector_signature_trace.kind[0]==3 && selector_signature_trace.function[0]==2);
        if(deny!=1) CHECK(selector_signature_trace.kind[1]==1 && selector_signature_trace.function[1]==0);
        if(!deny) CHECK(selector_signature_trace.fee[0]==11+sizeof(EffectBodyFunctionSignature) &&
            selector_signature_trace.fee[1]==12+sizeof(EffectBodyFunctionHead));
    }
    for(uint32_t deny=0;deny<=1;++deny) {
        selector_signature_trace=(SelectorSignatureTrace){.deny=deny};XrXirType type=(XrXirType)UINT32_MAX;
        CHECK(effect_capture_complete_type(&read,1,&type)==(deny?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(type==(deny?(XrXirType)UINT32_MAX:(XrXirType)256) && selector_signature_trace.calls==1 &&
            selector_signature_trace.kind[0]==3 && selector_signature_trace.function[0]==2);
    }
    for(uint32_t deny=0;deny<=3;++deny) {
        selector_signature_trace=(SelectorSignatureTrace){.deny=deny};EffectCaptureComplete complete={.rank=1};
        CHECK(effect_capture_complete_reference(&read,&node,0,1,&complete)==(deny?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(selector_signature_trace.calls==(deny?deny:3));
        CHECK(selector_signature_trace.kind[0]==3 && selector_signature_trace.function[0]==2);
        if(deny!=1) CHECK(selector_signature_trace.kind[1]==1 && selector_signature_trace.function[1]==0);
        if(deny==0 || deny==3) CHECK(selector_signature_trace.kind[2]==3 && selector_signature_trace.function[2]==2);
    }
    EffectBodyFunction *target=&material->functions[2];EffectBodyFunction saved=*target;
    for(uint32_t mode=0;mode<6;++mode) {
        selector_projection_storage(target,mode);selector_signature_trace=(SelectorSignatureTrace){0};
        XrXirType type=(XrXirType)UINT32_MAX;
        CHECK(effect_capture_complete_type(&read,1,&type)==XR_XIR_BAD_STRUCTURE && type==(XrXirType)UINT32_MAX);
        *target=saved;
    }
    SelectorCurrentPoll poll={.requested=true};
    CHECK(xr_compile_resources_begin_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    selector_signature_trace=(SelectorSignatureTrace){0};bool seeded=false;
    CHECK(effect_capture_reference_seed(&read,0,&seeded)==XR_XIR_BUDGET && !seeded && poll.calls);
    CHECK(xr_compile_resources_end_cancel(context.resources,selector_current_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
    selector_signature_trace=(SelectorSignatureTrace){0};
    CHECK(effect_capture_reference_seed(&read,0,&seeded)==XR_XIR_OK && seeded);
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK);
    CHECK(attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&context,baseline);
    printf("capture signature layout full%zu signature%zu saving%zu\n",sizeof(EffectBodyFunctionHead),
        sizeof(EffectBodyFunctionSignature),sizeof(EffectBodyFunctionHead)-sizeof(EffectBodyFunctionSignature));
    puts("capture signatures: literal REF identity, source full head, exact fees, all three callback refusals, unchanged outputs, full storage checks, cancellation/recovery and physical0 PASS");
}

int main(int argc,char **argv) {
    if(argc==2 && strcmp(argv[1],"resources")==0) {
        selector_raw_source_replay(true);return 0;
    }
    CHECK(argc==1);
    selector_capture_signature();
    selector_invocation_projection();
    selector_current_handoff();
    selector_complete_chain();
    selector_capture_support();
    selector_raw_source_replay(false);
    selector_scalar_domain();
    EdgeFixture fixture;pc_empty_fixture(&fixture,4);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    EffectBodyRequest body=effect_body_owned_request(&context,material);
    EffectBodyFunction *head=&material->functions[1];EffectBodyFunction saved=*head;
    CHECK(head->instruction_count==1 && head->run_count && head->run_capacity && head->runs);
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(unsigned mode=0;mode<3;++mode) {
        if(mode==1) { head->instruction_count=0;head->run_count=0; }
        if(mode==2) *head=saved;
        EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=UINT32_MAX;bool found=true;
        CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
        XrXirStatus actual=effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found);
        if(mode==1) {
            EffectBodyRecordCursor old;CHECK(effect_body_request_sequence_begin(&body,&old)==XR_XIR_OK);
            CHECK(effect_body_request_sequence_finish(&body,1,&old)==XR_XIR_BAD_STRUCTURE);
            fprintf(stderr,"actual zero instruction/nonzero owned storage selector status%u expected%u\n",actual,XR_XIR_BAD_STRUCTURE);
            CHECK(actual==XR_XIR_BAD_STRUCTURE);
        } else CHECK(actual==XR_XIR_OK && !found);
    }
    memset(&fixture,0xDC,sizeof(fixture));EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=0;bool found=true;
    CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
    CHECK(effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found)==XR_XIR_OK && !found);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && physical==live_bytes);
    effect_body_material_free(material);pc_release(&context,baseline);
    puts("actual opcode selector: zero instruction/storage inconsistency rejected; restoration, producer death, no callbacks and physical0");return 0;
}
