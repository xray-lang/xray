/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_sequential_vector_cases.h - Private complete receiving lifetimes
 */
#ifndef XIR_INVOCATION_SEQUENTIAL_VECTOR_CASES_H
#define XIR_INVOCATION_SEQUENTIAL_VECTOR_CASES_H
static void *sequential_old_nodes,*sequential_old_edges;
static unsigned sequential_dead;
static void *sequential_allocations[128];
static uint64_t sequential_work[128];
static size_t sequential_ordinals[128],sequential_allocation_count;
static unsigned sequential_deaths_at_allocation[128];
static XrCompileResources *sequential_resources;
static void sequential_release(void *raw) {
    if(raw==sequential_old_nodes)sequential_dead|=1;
    if(raw==sequential_old_edges)sequential_dead|=2;
}
static void sequential_allocate(void *raw) {
    CHECK(sequential_allocation_count<128 && sequential_resources);
    size_t at=sequential_allocation_count++;
    sequential_allocations[at]=raw;sequential_work[at]=sequential_resources->stats.work;
    sequential_ordinals[at]=rp_attempts-1;sequential_deaths_at_allocation[at]=sequential_dead;
}
static void sequential_observe(const XrXirCompileContext *context,const EffectInvocationFinal *source) {
    CHECK(!rp_release_observer && !rp_allocation_observer);
    sequential_old_nodes=source->nodes?(CompileAllocation *)source->nodes-1:NULL;
    sequential_old_edges=source->edges?(CompileAllocation *)source->edges-1:NULL;
    sequential_dead=0;sequential_allocation_count=0;sequential_resources=context->resources;
    rp_release_observer=sequential_release;rp_allocation_observer=sequential_allocate;
}
static void sequential_observe_close(void) {
    rp_release_observer=NULL;rp_allocation_observer=NULL;sequential_resources=NULL;
}
static size_t sequential_find(const void *payload) {
    const void *raw=(const CompileAllocation *)payload-1;
    for(size_t i=sequential_allocation_count;i>0;--i)if(sequential_allocations[i-1]==raw)return i-1;
    CHECK(false);return 0;
}
static unsigned char sequential_byte(size_t at,unsigned seed) {
    return (unsigned char)((at*43+seed)^(at>>2));
}
/* Arbitrary record bytes establish physical copying only, never authority. */
static void sequential_bytes(const void *payload,size_t bytes,unsigned seed) {
    const unsigned char *data=payload;
    for(size_t i=0;i<bytes;++i)CHECK(data[i]==sequential_byte(i,seed));
}
static XrXirStatus sequential_physical(const XrXirCompileContext *context,unsigned shape,
    uint64_t cut,size_t inject) {
    EffectBodyMaterial material={.resources=context->resources};
    EffectInvocationFinal source={.material=&material,.count=shape>=4?0:2,.edge_count=shape>=4?0:3,
        .capacity=shape==5?0:(shape==1 || shape==3)?2:5,
        .edge_capacity=shape==5?0:(shape==2 || shape==3)?3:7};
    XrXirStatus status=XR_XIR_OK;
    size_t node_bytes=(size_t)source.count*sizeof(*source.nodes),edge_bytes=(size_t)source.edge_count*sizeof(*source.edges);
    if(source.capacity)source.nodes=xir_compile_alloc(context,(size_t)source.capacity*sizeof(*source.nodes),&status);
    if(status==XR_XIR_OK && source.edge_capacity)source.edges=xir_compile_alloc(context,(size_t)source.edge_capacity*sizeof(*source.edges),&status);
    if(status==XR_XIR_OK){
        if(source.nodes)memset(source.nodes,0xCE,(size_t)source.capacity*sizeof(*source.nodes));
        if(source.edges)memset(source.edges,0xCE,(size_t)source.edge_capacity*sizeof(*source.edges));
        for(size_t i=0;i<node_bytes;++i)((unsigned char *)source.nodes)[i]=sequential_byte(i,19);
        for(size_t i=0;i<edge_bytes;++i)((unsigned char *)source.edges)[i]=sequential_byte(i,71);
        EffectInvocationFinal target=source;XrCompileResourceStats before=rp_stats(context);
        size_t attempts=rp_attempts;uint64_t original_limit=context->resources->limits.work;
        bool nodes=source.count<source.capacity,edges=source.edge_count<source.edge_capacity;
        uint64_t fee=sizeof(source)+(nodes?(node_bytes?1+node_bytes:0)+2*(sizeof(void *)+sizeof(uint32_t)):0)+
            (edges?(edge_bytes?1+edge_bytes:0)+2*(sizeof(void *)+sizeof(uint32_t)):0);
        if(cut!=UINT64_MAX)context->resources->limits.work=before.work+cut;
        if(inject!=SIZE_MAX){rp_fail_at=attempts+inject;rp_injected=false;}
        sequential_observe(context,&source);status=effect_invocation_final_vectors_consume(context,&source,&target);
        rp_fail_at=SIZE_MAX;sequential_observe_close();
        CHECK(!memcmp(&source,&target,sizeof(source)) && source.count<=source.capacity && source.edge_count<=source.edge_capacity);
        sequential_bytes(source.nodes,node_bytes,19);sequential_bytes(source.edges,edge_bytes,71);
        if(status==XR_XIR_OK){CHECK(source.capacity==source.count && source.edge_capacity==source.edge_count &&
                rp_stats(context).work-before.work==fee && rp_attempts-attempts==(size_t)(nodes && node_bytes)+(size_t)(edges && edge_bytes));
            CHECK(sequential_dead==(unsigned)(nodes?1:0)+(unsigned)(edges?2:0));
            if(nodes && node_bytes && edges && edge_bytes){size_t n=sequential_find(source.nodes),e=sequential_find(source.edges);
                CHECK(n<e && !sequential_deaths_at_allocation[n] && sequential_deaths_at_allocation[e]==1);}
        }else if(inject!=SIZE_MAX){CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+inject+1);
            CHECK(effect_invocation_final_vectors_consume(context,&source,&target)==XR_XIR_OK &&
                source.capacity==source.count && source.edge_capacity==source.edge_count);
            sequential_bytes(source.nodes,node_bytes,19);sequential_bytes(source.edges,edge_bytes,71);
        }else if(cut!=UINT64_MAX){CHECK(status==XR_XIR_BUDGET);RootParameterMark held=rp_mark();
            CHECK(effect_invocation_final_vectors_consume(context,&source,&target)==XR_XIR_BUDGET &&
                !memcmp(&source,&target,sizeof(source)) && rp_stats(context).work<=before.work+cut);rp_balanced(held);
            sequential_bytes(source.nodes,node_bytes,19);sequential_bytes(source.edges,edge_bytes,71);}
        /* The finite cap is never restored for a retry; cleanup alone follows. */
        CHECK(context->resources->limits.work==(cut==UINT64_MAX?original_limit:before.work+cut));
    }
    EffectInvocationFinal empty={0};effect_invocation_final_vectors_release(&source,&empty);return status;
}
static bool sequential_unknown_called;
static XrXirStatus sequential_unknown(const XrXirCompileContext *work,
    EffectInvocationWorkspace *owner,EffectInvocationFinal *target) {
    (void)work;(void)owner;(void)target;sequential_unknown_called=true;return XR_XIR_OK;
}
static XrXirStatus sequential_projection_fail(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,const uint32_t *selected,
    EffectFinalFunctions *result,void *receiver) {
    (void)work;(void)function;(void)selected;(void)result;
    CHECK(certificate->final->count==certificate->final->capacity &&
        certificate->final->edge_count==certificate->final->edge_capacity && receiver);
    ++*(unsigned *)receiver;return XR_XIR_IO;
}
static void sequential_binding_refusals(const XrXirCompileContext *context) {
    EffectBodyMaterial material={.resources=context->resources};EffectInvocationNode nodes[2];EffectInvocationEdge edges[3];
    EffectInvocationFinal source={.material=&material,.nodes=nodes,.edges=edges,.count=2,.capacity=2,.edge_count=3,.edge_capacity=3};
    EffectInvocationFinal target=source,original=source;XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    CHECK(effect_invocation_final_vectors_consume(NULL,&source,&target)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_final_vectors_consume(context,&source,&source)==XR_XIR_BAD_STRUCTURE);
    for(unsigned fault=0;fault<7;++fault){
        source=original;target=source;
        if(!fault)source.material=NULL;else if(fault==1)source.count=3;else if(fault==2)source.edge_count=4;
        else if(fault==3)source.nodes=NULL;else if(fault==4)source.edges=NULL;
        else if(fault==5)target.nodes=NULL;else target.capacity=3;
        CHECK(effect_invocation_final_vectors_consume(context,&source,&target)==XR_XIR_BAD_STRUCTURE);
    }
    source=original;target=source;CHECK(rp_attempts==attempts);XrCompileResourceStats after=rp_stats(context);
    CHECK(!memcmp(&before,&after,sizeof(before)));
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats other=rp_stats(&foreign);
    CHECK(effect_invocation_final_vectors_consume(&foreign,&source,&target)==XR_XIR_BAD_STRUCTURE);
    after=rp_stats(&foreign);CHECK(!memcmp(&other,&after,sizeof(other)));rp_owner_free(&foreign,other.live_bytes);
}
static bool sequential_sample;
static size_t sequential_vector_sites[2];
static uint64_t sequential_vector_cuts[10];
static XrXirStatus sequential_genuine(const XrXirCompileContext *context,size_t inject,bool fail_projection) {
    DriverLookupTrial trial;XrXirStatus status=driver_lookup_prepare(context,&trial);
    if(status!=XR_XIR_OK){driver_lookup_free(&trial);return status;}
    EffectInvocationFinal enlarged={0};status=final_vector_source(context,&trial.owner->final,&enlarged);
    if(status!=XR_XIR_OK){driver_lookup_free(&trial);return status;}
    effect_invocation_final_vectors_release(&trial.owner->final,&enlarged);trial.owner->final=enlarged;
    DriverLookupOracle oracle;driver_lookup_capture(trial.owner,&oracle);
    EffectInvocationFinal target=trial.owner->final;EffectInvocationSealRequest request={&trial.owner,&trial.staged,NULL};
    XrCompileResourceStats held=rp_stats(context);size_t attempts=rp_attempts;sequential_unknown_called=false;
    CHECK(effect_invocation_seal_receive(context,&request,&trial.certificate,NULL,sequential_unknown)==XR_XIR_BAD_STRUCTURE &&
        !sequential_unknown_called && !trial.certificate);
    CHECK(effect_invocation_final_vectors_driver_receive(context,trial.owner,&target)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=rp_stats(context);CHECK(rp_attempts==attempts && !memcmp(&held,&after,sizeof(held)));
    status=effect_invocation_driver_lookups_retire(trial.owner);
    if(status!=XR_XIR_OK){driver_lookup_free(&trial);return status;}
    unsigned calls=0;EffectInvocationProjectionSink sink={context->resources,6,&calls,sequential_projection_fail};
    size_t start=rp_attempts;sequential_observe(context,&trial.owner->final);
    if(inject!=SIZE_MAX){rp_fail_at=start+inject;rp_injected=false;}
    status=effect_invocation_seal_receive(context,&request,&trial.certificate,fail_projection?&sink:NULL,
        effect_invocation_final_vectors_driver_receive);
    rp_fail_at=SIZE_MAX;sequential_observe_close();
    if(status==XR_XIR_OK){CHECK(!trial.owner && !trial.staged && trial.certificate && sequential_dead==3);
        const EffectInvocationFinal *final=trial.certificate->final;size_t n=sequential_find(final->nodes),e=sequential_find(final->edges);
        CHECK(n<e && !sequential_deaths_at_allocation[n] && sequential_deaths_at_allocation[e]==1);
        if(sequential_sample){sequential_vector_sites[0]=sequential_ordinals[n]-start;sequential_vector_sites[1]=sequential_ordinals[e]-start;
            size_t ns=(size_t)final->count*sizeof(*final->nodes),es=(size_t)final->edge_count*sizeof(*final->edges);
            for(unsigned i=0;i<2;++i){uint64_t at=sequential_work[i?e:n],bytes=i?es:ns;
                sequential_vector_cuts[i*5]=at-1;sequential_vector_cuts[i*5+1]=at;sequential_vector_cuts[i*5+2]=at+bytes-1;
                sequential_vector_cuts[i*5+3]=at+bytes;sequential_vector_cuts[i*5+4]=at+bytes+2*(sizeof(void *)+sizeof(uint32_t))-1;}}
        EffectInvocationWorkspace received={.final=*final};driver_lookup_match(&received,&oracle);
        driver_lookup_drop_producers(&trial);EffectInvocationWorkspace replay={0};
        status=effect_invocation_read_open(context,trial.published,trial.certificate->declared,final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,trial.certificate->body);
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
        effect_invocation_read_close(&replay);
        if(status==XR_XIR_OK)status=effect_invocation_certificate_result_match(context,trial.certificate,&final->summary);
    }else{CHECK(trial.owner && trial.staged && !trial.certificate);driver_lookup_match(trial.owner,&oracle);
        if(inject!=SIZE_MAX)CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==start+inject+1);
        if(fail_projection)CHECK(status==XR_XIR_IO && calls==1 && sequential_dead==3);
    }
    driver_lookup_free(&trial);return status;
}
static void invocation_sequential_vector_controls(void) {
    uint64_t fee=sizeof(EffectInvocationFinal)+2+2*sizeof(EffectInvocationNode)+3*sizeof(EffectInvocationEdge)+
        4*(sizeof(void *)+sizeof(uint32_t));
    for(unsigned shape=0;shape<6;++shape){RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t live=rp_stats(&context).live_bytes;sequential_binding_refusals(&context);
        CHECK(sequential_physical(&context,shape,UINT64_MAX,SIZE_MAX)==XR_XIR_OK);rp_owner_free(&context,live);rp_balanced(mark);}
    for(uint64_t cut=0;cut<=fee;++cut){RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t live=rp_stats(&context).live_bytes;
        CHECK(sequential_physical(&context,0,cut,SIZE_MAX)==(cut==fee?XR_XIR_OK:XR_XIR_BUDGET));rp_owner_free(&context,live);rp_balanced(mark);}
    for(size_t inject=0;inject<2;++inject){RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t live=rp_stats(&context).live_bytes;CHECK(sequential_physical(&context,0,UINT64_MAX,inject)==XR_XIR_OUT_OF_MEMORY);
        rp_owner_free(&context,live);rp_balanced(mark);}
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    sequential_sample=true;CHECK(sequential_genuine(&context,SIZE_MAX,false)==XR_XIR_OK);sequential_sample=false;
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,live);rp_balanced(mark);
    for(unsigned mode=0;mode<13;++mode){mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(mode>=3)caps.work=sequential_vector_cuts[mode-3];context=rp_owner(caps);live=rp_stats(&context).live_bytes;
        CHECK(sequential_genuine(&context,mode<2?sequential_vector_sites[mode]:SIZE_MAX,mode==2)==
            (mode<2?XR_XIR_OUT_OF_MEMORY:mode==2?XR_XIR_IO:XR_XIR_BUDGET));rp_owner_free(&context,live);rp_balanced(mark);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;
        else caps.work=measured.work-minus;context=rp_owner(caps);live=rp_stats(&context).live_bytes;
        CHECK(sequential_genuine(&context,SIZE_MAX,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,live);rp_balanced(mark);}
    fprintf(stderr,"Private sequential Final: complete independent node/edge byte and actual fee oracles, all six exact/spare/empty shapes, old node storage dies before edge allocation, all binding/foreign/unknown receiver refusals, every work unit through%llu and same finite-owner prefix retry, both physical2 and genuine2 OOM, genuine ten allocation/copy/publication work cuts and post-receive projection IO cleanup; genuine Cell/Class complete solve/stage/seal with full node/edge/child/root oracle and independent Class/result after producer and Checked death, six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        (unsigned long long)fee,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_SEQUENTIAL_VECTOR_CASES_H
