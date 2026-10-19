/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_error_context_replay.c - Actual Error frontiers and independent full rows
 */
#define main error_context_retained_original_main
#include "test_xir_error_cfg_pending.c"
#undef main

/* The complete predecessor loop is independent of the selected-row walker.
 * Literal typed errors, opaque arms and caught errors remain separate oracles. */
static XrXirStatus context_original(EffectInvocationFlow *replay,ErrorInvocationContexts *contexts,
    const EffectInvocationCoverage *coverage) {
    EffectBodyRecordCursor cursor;
    XrXirStatus status=effect_body_request_sequence_begin(&replay->body,&cursor);
    for(uint32_t i=0;status==XR_XIR_OK && i<replay->function->instruction_count;++i) {
        if(!xir_compile_work(replay->read->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_instruction(&replay->body,
            replay->read->final->nodes[replay->node].body,i,&cursor,&storage,&op);
        if(status!=XR_XIR_OK) break;
        ErrorInvocationCall *call=&contexts->calls[contexts->nodes[replay->node].calls+i];
        if(call->frontier && (!call->handled || call->head>=contexts->link_count ||
            (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT &&
             contexts->links[call->head].next!=UINT32_MAX))) return XR_XIR_BAD_STRUCTURE;
        if(op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) continue;
        status=error_invocation_indirect_check(replay,contexts,coverage,i,op,&call->unknown);
        call->handled=true;
    }
    return status==XR_XIR_OK?effect_body_request_sequence_finish(&replay->body,
        replay->read->final->nodes[replay->node].body,&cursor):status;
}

static XrXirStatus context_refused_status;
static XrXirStatus context_refused(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    (void)request;(void)f;(void)i;(void)storage;(void)output;(void)fields;return context_refused_status;
}

static void context_reject_both(EffectInvocationFlow *replay,ErrorInvocationContexts *contexts,
    const EffectInvocationCoverage *coverage) {
    CHECK(context_original(replay,contexts,coverage)==XR_XIR_BAD_STRUCTURE);
    CHECK(error_invocation_context_replay(replay,contexts,coverage)==XR_XIR_BAD_STRUCTURE);
}

static void context_attacks(EffectInvocationFlow *replay,ErrorInvocationContexts *contexts,
    const EffectInvocationCoverage *coverage,uint32_t runs) {
    uint32_t f=replay->read->final->nodes[replay->node].body;
    EffectBodyFunction *function=&replay->read->final->material->functions[f];
    CHECK(f==0 && function->run_count==runs);
    ErrorInvocationCall *call=&contexts->calls[contexts->nodes[replay->node].calls];
    ErrorInvocationCall saved_call=*call;call->frontier=true;call->handled=false;
    context_reject_both(replay,contexts,coverage);*call=saved_call;
    call->frontier=true;call->handled=true;call->head=UINT32_MAX;
    context_reject_both(replay,contexts,coverage);*call=saved_call;
    for(uint32_t r=0;r<runs;++r) {
        EffectBodyRun *run=&function->runs[r],saved=*run;
        run->count=0;context_reject_both(replay,contexts,coverage);*run=saved;
        ++run->first;context_reject_both(replay,contexts,coverage);*run=saved;
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            run->mask|=UINT32_C(1)<<w;run->delta[w]=0;
            context_reject_both(replay,contexts,coverage);*run=saved;
        }
    }
    ++function->instruction_count;context_reject_both(replay,contexts,coverage);--function->instruction_count;
    const XrXirStatus statuses[]={XR_XIR_BUDGET,XR_XIR_OUT_OF_MEMORY,XR_XIR_BAD_TYPE};
    EffectBodyRequest original=replay->body;replay->body.instruction=context_refused;
    for(uint32_t i=0;i<3;++i) {
        context_refused_status=statuses[i];uint64_t live_before=pc_stats(replay->read->work).live_bytes;
        CHECK(error_invocation_context_replay(replay,contexts,coverage)==statuses[i]);
        CHECK(pc_stats(replay->read->work).live_bytes==live_before);
    }
    replay->body=original;CHECK(error_invocation_context_replay(replay,contexts,coverage)==XR_XIR_OK);
}

static void context_compare(ErrorFlow *flow,uint32_t runs) {
    ErrorInvocationContexts *old=NULL,*got=NULL;
    CHECK(error_invocation_contexts_new(flow,&old)==XR_XIR_OK && old);
    CHECK(error_invocation_contexts_new(flow,&got)==XR_XIR_OK && got && got->count==old->count);
    EffectInvocationCertificate *certificate=flow->effects->invocations;EffectInvocationRead read={0};
    CHECK(effect_invocation_read_open(flow->remaining,flow->effects,certificate->declared,certificate->final,&read)==XR_XIR_OK);
    for(uint32_t n=0;n<read.final->count;++n) {
        EffectInvocationFlow replay={.read=&read,.node=n,.instruction=UINT32_MAX};
        CHECK(effect_invocation_origins(&replay)==XR_XIR_OK);EffectInvocationCoverage coverage={0};
        if(read.producer_enabled) CHECK(effect_invocation_coverage_values(&replay,&coverage)==XR_XIR_OK);
        uint32_t count=replay.function->instruction_count;
        for(uint32_t i=0;i<count;++i) {
            old->calls[old->nodes[n].calls+i].unknown=false;got->calls[got->nodes[n].calls+i].unknown=false;
        }
        XrCompileResourceStats before=pc_stats(flow->remaining);size_t allocated=attempts;
        CHECK(context_original(&replay,old,&coverage)==XR_XIR_OK);
        CHECK(error_invocation_context_replay(&replay,got,&coverage)==XR_XIR_OK);
        CHECK(pc_stats(flow->remaining).live_bytes==before.live_bytes && attempts==allocated);
        for(uint32_t i=0;i<count;++i) {
            ErrorInvocationCall a=old->calls[old->nodes[n].calls+i],b=got->calls[got->nodes[n].calls+i];
            CHECK(a.head==b.head && a.unknown==b.unknown && a.handled==b.handled && a.frontier==b.frontier);
        }
        if(read.final->nodes[n].body==0) context_attacks(&replay,got,&coverage,runs);
        xr_compile_resources_free(coverage.memory);xr_compile_resources_free(replay.memory);
    }
    effect_invocation_read_close(&read);error_invocation_contexts_free(old);error_invocation_contexts_free(got);
}

static void context_case(uint32_t mode,uint32_t runs) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    InvocationErrorFixture fixture;ei_fixture(&fixture,mode);
    XrXirInstruction ops[9]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64,.immediate=1},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=97},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=98},
        {.op=XR_XIR_RETURN}};
    uint32_t count=runs*2-1;ops[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    XrXirBlock block={.count=count};fixture.edge.functions[0].instructions=ops;
    fixture.edge.functions[0].instruction_count=count;fixture.edge.functions[0].blocks=&block;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.edge.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xa5,sizeof(fixture));memset(ops,0xa5,sizeof(ops));ei_golden(effects,mode);
    ErrorFlow flow={.effects=effects,.remaining=&context,.body=effect_body_owned_request(&context,effects->invocations->body)};
    context_compare(&flow,runs);ei_golden(effects,mode);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);CHECK(!live && !live_bytes);
}

int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if(argc==2) {
        for(uint32_t mode=0;mode<EI_CASES;++mode) {ei_faults(mode);ei_limits(mode);}
        puts("Error context original six OOM/three-axis families and physical0 PASS");return 0;
    }
    for(uint32_t mode=0;mode<EI_CASES;++mode) for(uint32_t runs=1;runs<=5;++runs) context_case(mode,runs);
    puts("Error context:30 literal typed/caught/opaque graphs, independent full rows, all10 columns/EOF/frontiers/status failures and physical0 PASS");
    return 0;
}
