/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_end_words.inc.c - Atomic retirement of real backend owner words
 */
static XrXirViewEndWord call_view_end_row(const XrXirViewEndBatch *batch,uint32_t index) {
    XrXirViewEndWord row;
    memcpy(&row,(const unsigned char *)batch->words+(size_t)index*sizeof(row),sizeof(row));return row;
}
static XrXirValueStatus call_view_end_validate(XrXirCall *call,const XrXirViewEndBatch *batch,
    XrXirValueAdmission *admission) {
    for (uint32_t i=0;i<batch->count;++i) {
        if (!call_cell_work(call,5)) return XR_XIR_VALUE_LIMIT;
        XrXirViewEndWord row=call_view_end_row(batch,i);
        if (batch->frame_bytes<sizeof(int64_t) || row.offset>batch->frame_bytes-sizeof(int64_t) ||
            row.offset%sizeof(int64_t) || !xr_xir_compile_type_arena_borrowed(admission->arena,row.type))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValue value={(uint32_t)row.type,0,xr_xir_scalar_load(batch->frame,row.offset)};
        if (value.payload && !xr_xir_value_argument(&value,admission->arena,row.type)) return XR_XIR_VALUE_BAD_ARGUMENT;
        bool mutable=(xr_xir_compile_type_arena_borrowed(admission->arena,row.type)&XR_XIR_VIEW_MUTABLE)!=0;
        for (uint32_t j=0;j<i;++j) {
            if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
            XrXirViewEndWord previous=call_view_end_row(batch,j);
            if (row.offset==previous.offset || (mutable && value.payload && row.type==previous.type &&
                value.payload==xr_xir_scalar_load(batch->frame,previous.offset))) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
    }
    for (CallViewBinding *binding=call->top->views;binding;binding=binding->next)
        if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
    return call_cell_work(call,(uint64_t)batch->count*3u) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_end_words(XrXirCallView *view,const XrXirViewEndBatch *input) {
    if (!call_view_move_current(view) || !input) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirViewEndBatch batch=*input;XrXirCall *call=view->activation;
    if (!batch.count || !batch.words || batch.count>batch.frame_bytes/sizeof(int64_t) ||
        !call_view_move_span(call->top,batch.frame,batch.frame_bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes=(uint64_t)batch.count*sizeof(*batch.words);
    uintptr_t frame=(uintptr_t)batch.frame,rows=(uintptr_t)batch.words;
    if (bytes>SIZE_MAX || bytes>UINTPTR_MAX-rows || batch.frame_bytes>UINTPTR_MAX-frame ||
        (rows<frame+batch.frame_bytes && frame<rows+bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    call->admitting=true;
    XrXirValueStatus status=call_view_end_validate(call,&batch,xr_xir_call_admission(view));
    if (status==XR_XIR_VALUE_OK) {
        for (uint32_t i=0;i<batch.count;++i) {
            XrXirViewEndWord row=call_view_end_row(&batch,i);
            xr_xir_owned_slot_clear(batch.frame,row.offset);
        }
        XrXirValueStatus collected=call_view_collect(call,false);
        XR_CHECK(collected==XR_XIR_VALUE_OK,"prepaid backend owner retirement cannot fail");
    }
    call->admitting=false;return status;
}
