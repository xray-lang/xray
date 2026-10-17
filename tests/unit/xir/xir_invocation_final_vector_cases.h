/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_final_vector_cases.h - Real immutable vector extent controls
 */
#ifndef XIR_INVOCATION_FINAL_VECTOR_CASES_H
#define XIR_INVOCATION_FINAL_VECTOR_CASES_H
static XrXirStatus final_vector_source(const XrXirCompileContext *context,
    const EffectInvocationFinal *real,EffectInvocationFinal *source) {
    *source=*real;source->nodes=NULL;source->edges=NULL;
    source->capacity=real->count+7;source->edge_capacity=real->edge_count+9;
    XrXirStatus status=XR_XIR_OK;
    source->nodes=xir_compile_alloc(context,(size_t)source->capacity*sizeof(*source->nodes),&status);
    if(status==XR_XIR_OK)source->edges=xir_compile_alloc(context,(size_t)source->edge_capacity*sizeof(*source->edges),&status);
    if(status!=XR_XIR_OK){effect_invocation_final_vectors_release(source,real);*source=(EffectInvocationFinal){0};return status;}
    memset(source->nodes,0xa5,(size_t)source->capacity*sizeof(*source->nodes));
    memset(source->edges,0xa5,(size_t)source->edge_capacity*sizeof(*source->edges));
    memcpy(source->nodes,real->nodes,(size_t)real->count*sizeof(*source->nodes));
    if(real->edge_count)memcpy(source->edges,real->edges,(size_t)real->edge_count*sizeof(*source->edges));
    return XR_XIR_OK;
}
static void final_vector_same(const EffectInvocationFinal *packed,const EffectInvocationFinal *real) {
    CHECK(packed->count==real->count && packed->edge_count==real->edge_count &&
        packed->capacity==packed->count && packed->edge_capacity==packed->edge_count &&
        packed->material==real->material && packed->physical==real->physical && packed->cells==real->cells &&
        !memcmp(packed->nodes,real->nodes,(size_t)real->count*sizeof(*real->nodes)));
    if(real->edge_count)CHECK(!memcmp(packed->edges,real->edges,(size_t)real->edge_count*sizeof(*real->edges)));
}
static XrXirStatus final_vector_operation(const XrXirCompileContext *context,
    const EffectInvocationFinal *source,const EffectInvocationFinal *real) {
    EffectInvocationFinal packed=*source;unsigned char held[sizeof(packed)];memcpy(held,&packed,sizeof(packed));
    XrXirStatus status=effect_invocation_final_vectors_copy(context,source,&packed);
    if(status==XR_XIR_OK){final_vector_same(&packed,real);effect_invocation_final_vectors_release(&packed,source);}
    else CHECK(!memcmp(held,&packed,sizeof(packed)));
    return status;
}
static XrXirStatus final_vector_whole(const XrXirCompileContext *context) {
    XrXirEffects *effects=NULL;XrXirStatus status=row_data_fixture(context,&effects,NULL,NULL);
    EffectInvocationFinal source={0};
    if(status==XR_XIR_OK)status=final_vector_source(context,effects->invocations->final,&source);
    if(status==XR_XIR_OK)status=final_vector_operation(context,&source,effects->invocations->final);
    if(source.material)effect_invocation_final_vectors_release(&source,effects->invocations->final);
    xr_xir_compile_effects_free(effects);return status;
}
static void final_vector_axes(void) {
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass) {
        RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(final_vector_whole(&context)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(measured.allocated_bytes==stats.allocated_bytes &&
            measured.peak_bytes==stats.peak_bytes && measured.work==stats.work);
        rp_owner_free(&context,live);rp_balanced(mark);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        RootParameterMark mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;
        else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(final_vector_whole(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(mark);
    }
    fprintf(stderr,"Final vector whole-fixture six axes allocated%llu/peak%llu/work%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
static void invocation_final_vector_controls(void) {
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirEffects *effects=NULL;CHECK(row_data_fixture(&context,&effects,NULL,NULL)==XR_XIR_OK);
    const EffectInvocationFinal *real=effects->invocations->final;EffectInvocationFinal source={0};
    CHECK(real->count && real->edge_count && real->count==real->capacity && real->edge_count==real->edge_capacity);
    CHECK(final_vector_source(&context,real,&source)==XR_XIR_OK);
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    CHECK(final_vector_operation(&context,&source,real)==XR_XIR_OK);
    size_t sites=rp_attempts-attempts;CHECK(sites==2);rp_balanced(held);
    for(size_t at=0;at<sites;++at) {
        attempts=rp_attempts;rp_fail_at=attempts+at;rp_injected=false;XrCompileResources *identity=context.resources;
        XrXirStatus status=final_vector_operation(&context,&source,real);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && identity==context.resources);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
        CHECK(final_vector_operation(&context,&source,real)==XR_XIR_OK && identity==context.resources);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
    }
    uint64_t spare_bytes=(uint64_t)(source.capacity-source.count)*sizeof(*source.nodes)+
        (uint64_t)(source.edge_capacity-source.edge_count)*sizeof(*source.edges);
    uint64_t before_packing=rp_stats(&context).live_bytes;
    EffectInvocationFinal packed=source;
    CHECK(effect_invocation_final_vectors_copy(&context,&source,&packed)==XR_XIR_OK);
    final_vector_same(&packed,real);
    effect_invocation_final_vectors_release(&source,real);source=(EffectInvocationFinal){0};
    CHECK(spare_bytes && rp_stats(&context).live_bytes==before_packing-spare_bytes);
    final_vector_same(&packed,real);
    EffectInvocationFinal alias=packed;attempts=rp_attempts;rp_fail_at=attempts;rp_injected=false;
    CHECK(effect_invocation_final_vectors_copy(&context,&packed,&alias)==XR_XIR_OK && !rp_injected &&
        rp_attempts==attempts && alias.nodes==packed.nodes && alias.edges==packed.edges);
    rp_fail_at=SIZE_MAX;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    alias=packed;CHECK(effect_invocation_final_vectors_copy(&foreign,&packed,&alias)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats foreign_after=rp_stats(&foreign);CHECK(!memcmp(&foreign_before,&foreign_after,sizeof(foreign_before)));
    rp_owner_free(&foreign,foreign_before.live_bytes);
    alias=*real;attempts=rp_attempts;
    CHECK(effect_invocation_final_vectors_copy(&context,&packed,&alias)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    effect_invocation_final_vectors_release(&packed,real);xr_xir_compile_effects_free(effects);
    rp_owner_free(&context,live);rp_balanced(mark);
    fprintf(stderr,"Final vector real complete node/edge oracle, poisoned spare tails excluded, producer vector death, armed allocation-free exact transfer, foreign ledger/occupied output and every2 OOM same-owner retry PASS\n");
    fprintf(stderr,"Final vector exact live reclamation%llu bytes, all actual children remain live through independent oracle PASS\n",(unsigned long long)spare_bytes);
    final_vector_axes();
}
#endif // XIR_INVOCATION_FINAL_VECTOR_CASES_H
