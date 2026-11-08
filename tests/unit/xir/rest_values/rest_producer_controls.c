/* Current capture words and late cleanup edges are received independently. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_internal.h"
#include "xir/xxir_effects.c"
#include "../xir_final_origin_receiving_cases.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"rest producer %d: %s\n",__LINE__,#x);exit(1);}}while(0)
extern void rest_producer_fail_next(void);
extern void rest_producer_fault_clear(void);
extern bool rest_producer_fault_observed(void);
extern void rest_producer_physical(size_t *blocks,size_t *bytes);

static uint64_t rest_producer_held(const XrXirCompileContext *work) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(work->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirStatus rest_producer_begin(const XrXirCompileContext *work,const XrXirModule *module,
    XrXirEffects *effects,EffectGraph *graph,EffectInvocationWorkspace **owner,XrXirEffects *seed) {
    XrXirCompileContext remaining=*work;EffectInvocationDeclaredBounds *bounds=NULL;
    *seed=(XrXirEffects){.resources=work->resources,.count=module->function_count,.input_facts=effects->input_facts};
    XrXirStatus status=XR_XIR_OK;uint32_t count=seed->count;
    seed->outcomes_work.functions=xir_compile_calloc(work,count,sizeof(*seed->outcomes_work.functions),&status);
    seed->outcomes_work.task_creation=xir_compile_calloc(work,count,sizeof(*seed->outcomes_work.task_creation),&status);
    seed->outcomes_work.root=xir_compile_calloc(work,count,sizeof(*seed->outcomes_work.root),&status);
    seed->outcomes_work.root_witnesses=xir_compile_calloc(work,count,sizeof(*seed->outcomes_work.root_witnesses),&status);
    seed->outcomes_work.unresolved_witnesses=xir_compile_calloc(work,count,sizeof(*seed->outcomes_work.unresolved_witnesses),&status);
    if(status==XR_XIR_OK)status=effect_graph_build(module,seed,graph,&remaining);
    if(status==XR_XIR_OK)status=effect_parameters_derive(module,seed,graph,&remaining);
    if(status==XR_XIR_OK)status=effect_invocation_publication_bounds(work,module,seed,&bounds);
    if(status==XR_XIR_OK)status=effect_invocations_receive(work,module,seed,graph,bounds,owner);
    effect_invocation_bounds_free(bounds);
    return status;
}
static void rest_producer_seed_free(XrXirEffects *seed) {
    effect_parameters_free(seed);
    xr_compile_resources_free(seed->outcomes_work.functions);xr_compile_resources_free(seed->outcomes_work.task_creation);
    xr_compile_resources_free(seed->outcomes_work.root);xr_compile_resources_free(seed->outcomes_work.root_witnesses);
    xr_compile_resources_free(seed->outcomes_work.unresolved_witnesses);*seed=(XrXirEffects){0};
}
static XrXirStatus rest_producer_receive(const XrXirCompileContext *work,EffectInvocationCertificate *certificate) {
    EffectBodyRequest body=effect_body_owned_request(work,certificate->body);
    XrXirEffects borrowed={.invocations=certificate};
    return final_origin_receive(work,&borrowed,&body);
}
static bool rest_producer_cancel(const void *context) {return *(const bool *)context;}
static void rest_producer_failures(const XrXirCompileContext *work,const XrXirModule *module,XrXirEffects *effects) {
    uint64_t before=rest_producer_held(work);size_t blocks=0,bytes=0;
    rest_producer_physical(&blocks,&bytes);
    for(unsigned mode=0;mode<2;++mode) {
        EffectGraph graph={0};EffectInvocationWorkspace *owner=NULL;XrXirEffects seed={0};
        CHECK(rest_producer_begin(work,module,effects,&graph,&owner,&seed)==XR_XIR_OK);
        CHECK(effect_invocations_discover(owner)==XR_XIR_OK);
        uint32_t nodes=owner->final.count,edges=owner->final.edge_count;
        EffectParameterSummary parameters=owner->final.parameter_summary;
        CHECK(parameters.resources==work->resources && parameters.rows && parameters.values && parameters.total);
        XrXirEffectParameter first=parameters.values[0];
        CHECK(effect_capture_build(work,&owner->final,owner)==XR_XIR_OK);
        CHECK(owner->final.count>nodes||owner->final.edge_count>edges);
        CHECK(!owner->final.capture.facts&&!owner->final.capture.count);
        CHECK(owner->final.parameter_summary.resources==parameters.resources &&
            owner->final.parameter_summary.rows==parameters.rows && owner->final.parameter_summary.values==parameters.values &&
            owner->final.parameter_summary.count==parameters.count && owner->final.parameter_summary.total==parameters.total);
        CHECK(parameters.values[0].kind==first.kind && parameters.values[0].uses==first.uses);
        if(!mode) {
            rest_producer_fail_next();
            CHECK(effect_invocations_solve(owner)==XR_XIR_OUT_OF_MEMORY&&rest_producer_fault_observed());
            rest_producer_fault_clear();
        } else {
            bool cancelled=true;XrCompileRequestControl control={0};
            const XrCompileRequestOptions options={.cancel_poll=rest_producer_cancel,.cancel_context=&cancelled};
            CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
            XirEffectInput input={.module=module,.construction=effects->input_facts->construction,.control=&control};
            CHECK(effect_input_facts_match(work,input,graph.input_facts)==XR_XIR_CANCELLED);
            /* This is the next actual current-input poll, not an internal
             * cancellation point inside the callback-free equation solver. */
            CHECK(control.status==XR_COMPILE_RESOURCE_CANCELLED);
        }
        effect_invocation_free(owner);effect_graph_free(&graph);rest_producer_seed_free(&seed);
        size_t after_blocks=0,after_bytes=0;rest_producer_physical(&after_blocks,&after_bytes);
        CHECK(rest_producer_held(work)==before&&after_blocks==blocks&&after_bytes==bytes);
    }
    EffectGraph graph={0};EffectInvocationWorkspace *owner=NULL;XrXirEffects seed={0};
    CHECK(rest_producer_begin(work,module,effects,&graph,&owner,&seed)==XR_XIR_OK);
    CHECK(effect_invocations_solve(owner)==XR_XIR_OK);
    effect_invocation_free(owner);effect_graph_free(&graph);rest_producer_seed_free(&seed);
    size_t after_blocks=0,after_bytes=0;rest_producer_physical(&after_blocks,&after_bytes);
    CHECK(rest_producer_held(work)==before&&after_blocks==blocks&&after_bytes==bytes);
    puts("Post-growth rebuild OOM and next current-input cancellation retire all owners; fresh retry PASS");
}
static uint32_t rest_producer_keys(const XrXirCompileContext *work,EffectInvocationFinal *final,
    uint32_t first,uint32_t *selected,uint32_t *captured) {
    EffectCaptureRead read={.work=work,.final=final};CHECK(effect_capture_frame(&read)==XR_XIR_OK);
    uint32_t keys=0;*selected=UINT32_MAX;*captured=UINT32_MAX;
    for(uint32_t e=first;e<final->edge_count;++e) {
        EffectInvocationEdge edge=final->edges[e];EffectCaptureNode caller={0};
        CHECK(effect_capture_node(&read,edge.caller,&caller)==XR_XIR_OK);
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        CHECK(effect_body_request_operation(&read.body,final->nodes[edge.caller].body,
            edge.instruction,&storage,&op)==XR_XIR_OK);
        if(op->op!=XR_XIR_CLEANUP_REGISTER)continue;
        CHECK(!edge.latent&&edge.producer==UINT32_MAX&&op->immediate==final->nodes[edge.target].body);
        for(uint32_t p=0;p<op->args[1];++p) {
            uint32_t actual=caller.head.operands[op->args[0]+p];XrXirType type=0;
            CHECK(effect_body_request_type(&read.body,final->nodes[edge.caller].body,actual,&type)==XR_XIR_OK);
            if(!effect_physical_origin(read.body.domain.types,type))continue;
            const uint64_t *input=final->nodes[edge.target].input+(size_t)p*caller.basis.row_words;
            for(uint32_t w=0;w<caller.basis.fn_words;++w) {
                uint64_t word=0;CHECK(effect_capture_word(&read,(EffectCaptureFact){edge.caller,actual,w,0,0},&word)==XR_XIR_OK);
                CHECK(word==input[w]);
                if(w>=caller.basis.origin_words&&word)*captured=e;
            }
            const EffectCaptureFact *raw=NULL;
            CHECK(effect_capture_find(&read,(EffectCaptureFact){edge.caller,actual,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0},&raw)==XR_XIR_OK);
            if(!raw)*selected=e;
        }
        ++keys;
        printf("Cleanup key edge%u origin%u fn%u bindings%u\n",e,caller.basis.origin_words,caller.basis.fn_words,final->binding_count);
    }
    return keys;
}
static void rest_producer_attacks(const XrXirCompileContext *work,EffectInvocationCertificate *certificate,uint32_t selected,uint32_t captured) {
    EffectInvocationFinal *final=certificate->final;
    CHECK(selected<final->edge_count&&captured<final->edge_count);
    size_t edge_bytes=(size_t)final->edge_count*sizeof(*final->edges);
    EffectInvocationEdge *edges=xr_malloc(edge_bytes);CHECK(edges);memcpy(edges,final->edges,edge_bytes);
    uint32_t *heads=xr_malloc((size_t)final->count*sizeof(*heads));CHECK(heads);
    for(uint32_t n=0;n<final->count;++n)heads[n]=final->nodes[n].edge_head;
    uint32_t count=final->edge_count;
    /* Remove one actual edge and repair only the remaining list ordinals. */
    EffectInvocationEdge removed=final->edges[selected];
    for(uint32_t n=0;n<final->count;++n) {
        uint32_t h=final->nodes[n].edge_head;
        if(h==selected)h=removed.next;
        final->nodes[n].edge_head=h!=UINT32_MAX&&h>selected?h-1:h;
    }
    memmove(final->edges+selected,final->edges+selected+1,(size_t)(count-selected-1)*sizeof(*edges));
    --final->edge_count;
    for(uint32_t e=0;e<final->edge_count;++e) {
        uint32_t next=final->edges[e].next,consumer=final->edges[e].consumer_next;
        if(next==selected)next=removed.next;
        if(consumer==selected)consumer=removed.consumer_next;
        final->edges[e].next=next!=UINT32_MAX&&next>selected?next-1:next;
        final->edges[e].consumer_next=consumer!=UINT32_MAX&&consumer>selected?consumer-1:consumer;
    }
    EffectBodyRequest body=effect_body_owned_request(work,final->material);
    for(uint32_t e=0;e<final->edge_count;++e)CHECK(effect_final_edge_payload(work,&body,final,e)==XR_XIR_OK);
    CHECK(effect_final_inventory_edges(work,final->material,final)==XR_XIR_BAD_STRUCTURE);
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_BAD_STRUCTURE);
    memcpy(final->edges,edges,edge_bytes);final->edge_count=count;
    for(uint32_t n=0;n<final->count;++n)final->nodes[n].edge_head=heads[n];
    xr_free(heads);xr_free(edges);CHECK(rest_producer_receive(work,certificate)==XR_XIR_OK);

    EffectInvocationEdge edge=final->edges[selected];EffectCaptureRead read={.work=work,.final=final};
    CHECK(effect_capture_frame(&read)==XR_XIR_OK);EffectCaptureNode caller={0};
    CHECK(effect_capture_node(&read,edge.caller,&caller)==XR_XIR_OK);
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    CHECK(effect_body_request_operation(&read.body,final->nodes[edge.caller].body,edge.instruction,&storage,&op)==XR_XIR_OK);
    uint32_t value=caller.head.operands[op->args[0]];
    CHECK(final->capture.count<final->capture.capacity);
    EffectCaptureFact fake={edge.caller,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0};uint32_t at=0;
    while(at<final->capture.count&&effect_capture_before(&final->capture.facts[at],&fake))++at;
    memmove(final->capture.facts+at+1,final->capture.facts+at,(size_t)(final->capture.count-at)*sizeof(fake));
    final->capture.facts[at]=fake;++final->capture.count;
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_BAD_STRUCTURE);
    --final->capture.count;memmove(final->capture.facts+at,final->capture.facts+at+1,(size_t)(final->capture.count-at)*sizeof(fake));
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_OK);

    EffectInvocationNode *target=&final->nodes[final->edges[captured].target];
    EffectInvocationBasis basis={0};EffectBodyFunctionDimensions root={0};
    CHECK(effect_body_request_dimensions(&body,target->root,&root)==XR_XIR_OK);
    CHECK(effect_invocation_basis_parameters_read(work,final,root.parameter_count,&basis)==XR_XIR_OK);
    uint64_t *changed=NULL;
    for(uint32_t p=0;p<target->parameter_count&&!changed;++p)
        for(uint32_t w=basis.origin_words;w<basis.fn_words;++w)
            if(target->input[(size_t)p*basis.row_words+w]){changed=&target->input[(size_t)p*basis.row_words+w];break;}
    CHECK(changed);uint64_t saved=*changed;*changed^=saved&(~saved+1);
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_BAD_STRUCTURE);*changed=saved;
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_OK);
}
void rest_producer_controls(XrXirArtifact *artifact) {
    const XrXirCompileContext *work=xr_xir_compile_artifact_context(artifact);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint64_t before=rest_producer_held(work);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    CHECK(rest_producer_receive(work,effects->invocations)==XR_XIR_OK);
    uint64_t retained=rest_producer_held(work);
    rest_producer_failures(work,module,effects);
    EffectGraph graph={0};EffectInvocationWorkspace *owner=NULL;XrXirEffects seed={0};
    CHECK(rest_producer_begin(work,module,effects,&graph,&owner,&seed)==XR_XIR_OK);
    CHECK(effect_invocations_discover(owner)==XR_XIR_OK);uint32_t first=owner->final.edge_count;
    CHECK(effect_invocation_origins_finalize(owner)==XR_XIR_OK);
    uint32_t selected=UINT32_MAX,captured=UINT32_MAX;
    uint32_t keys=rest_producer_keys(work,&owner->final,first,&selected,&captured);
    printf("Cleanup discovery edges %u final %u late %u opaque %u captured %u\n",first,owner->final.edge_count,keys,selected,captured);
    CHECK(keys>=2&&selected!=UINT32_MAX&&captured!=UINT32_MAX);
    printf("Late cleanup full keys %u with a genuine nonzero capture column\n",keys);
    CHECK(effect_invocation_iterator_residual(owner)==XR_XIR_OK&&effect_invocations_propagate(owner)==XR_XIR_OK);
    EffectFinalFunctions *solution=NULL;EffectInvocationCertificate *certificate=NULL;
    CHECK(effect_invocation_stage(owner,&solution)==XR_XIR_OK);
    EffectInvocationSealRequest seal={&owner,&solution,NULL};
    CHECK(effect_invocation_seal(work,&seal,&certificate,NULL)==XR_XIR_OK&&!owner&&!solution);
    CHECK(rest_producer_receive(work,certificate)==XR_XIR_OK);
    rest_producer_attacks(work,certificate,selected,captured);
    effect_invocation_certificate_free(certificate);effect_graph_free(&graph);rest_producer_seed_free(&seed);
    CHECK(rest_producer_held(work)==retained);
    xr_xir_compile_effects_free(effects);CHECK(rest_producer_held(work)==before);
}
