/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_cell_types.inc.c - Physical type columns for one Cell replay
 */
static void effect_cell_body_projection_drop(EffectCellBodyAdapter *adapter) {
    if (!adapter) return;
    xr_compile_resources_free(adapter->projection_offsets);
    *adapter=(EffectCellBodyAdapter){0};
}

/* Projection starts only after every physical run has been written. Each
 * independent provider operation owns a fresh copy of type ordinals, never
 * a previous Cell proof, Class identity or executable admission conclusion. */
static XrXirStatus effect_cell_body_project(EffectCellBodyAdapter *adapter) {
    if (!adapter || !effect_cell_body_adapter_valid(adapter->work,adapter) ||
        adapter->projection_body || adapter->projection_offsets || adapter->projection_types ||
        adapter->projection_values) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(&adapter->request)) return XR_XIR_OK;
    XrXirStatus status=effect_body_request_sequence_binding(&adapter->request);
    if (status!=XR_XIR_OK) return status;
    const XrXirCompileContext *work=adapter->work;
    const EffectBodyMaterial *body=adapter->request.owner;uint64_t values=0;
    uint32_t functions=effect_body_count(body);
    if (!functions) return XR_XIR_OK;
    for (uint32_t f=0;f<functions;++f) {
        EffectBodyFunctionHead head={0};status=effect_body_request_head(&adapter->request,f,&head);
        if (status!=XR_XIR_OK) return status;
        uint64_t count=(uint64_t)head.parameter_count+head.instruction_count;
        if (count>UINT32_MAX || count>UINT64_MAX-values) return XR_XIR_BUDGET;
        if (!xir_compile_work(work,2+sizeof(values))) return XR_XIR_BUDGET;
        values+=count;
    }
    uint64_t directory=((uint64_t)functions+1)*sizeof(uint64_t);
    if (directory>SIZE_MAX || values>(SIZE_MAX-directory)/sizeof(XrXirType)) return XR_XIR_BUDGET;
    uint64_t *offsets=xir_compile_alloc(work,(size_t)(directory+values*sizeof(XrXirType)),&status);
    if (!offsets) return status;
    XrXirType *types=(XrXirType *)((uint8_t *)offsets+(size_t)directory);uint64_t at=0;
    for (uint32_t f=0;f<functions && status==XR_XIR_OK;++f) {
        const EffectBodyFunction *function=&body->functions[f];
        if (!xir_compile_work(work,sizeof(*offsets))) { status=XR_XIR_BUDGET;break; }
        offsets[f]=at;
        for (uint32_t p=0;p<function->parameter_count;++p) {
            if (at>=values || !function->parameters) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (!xir_compile_work(work,2+sizeof(*types))) { status=XR_XIR_BUDGET;break; }
            types[at++]=function->parameters[p];
        }
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(cursor))) status=XR_XIR_BUDGET;
        for (uint32_t r=0;r<function->run_count && status==XR_XIR_OK;++r) {
            const EffectBodyRun *ref=NULL;
            status=effect_body_range_ref(&adapter->request.read,function,&cursor,&ref);
            if (status!=XR_XIR_OK) break;
            for (uint32_t i=ref->first;i<ref->first+ref->count;++i) {
                uint32_t type=0;status=effect_body_run_word(&adapter->request.read,ref,i,1,&type);
                if (status!=XR_XIR_OK) break;
                if (at>=values) { status=XR_XIR_BAD_STRUCTURE;break; }
                if (!xir_compile_work(work,1+sizeof(*types))) { status=XR_XIR_BUDGET;break; }
                types[at++]=(XrXirType)type;
            }
        }
        if (status==XR_XIR_OK && (cursor.run!=function->run_count || cursor.first!=function->instruction_count))
            status=XR_XIR_BAD_STRUCTURE;
    }
    if (status==XR_XIR_OK && at!=values) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*offsets)+4*sizeof(void *)+sizeof(values)))
        status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(offsets);return status; }
    offsets[functions]=at;adapter->projection_body=body;adapter->projection_offsets=offsets;
    adapter->projection_types=types;adapter->projection_values=values;return XR_XIR_OK;
}
