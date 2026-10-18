/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_moves.inc.c - Scoped aggregate construction and PHI publication
 */
static bool call_view_move_current(XrXirCallView *view) {
    return xr_xir_call_admission(view) && view->phase==XR_XIR_CALL_NORMAL &&
        !view->activation->admitting && !view->activation->top->return_staged &&
        xr_xir_call_execution_status(view)==XR_XIR_CALL_READY;
}
static bool call_view_move_span(const CallFrame *frame,const void *pointer,uint64_t bytes) {
    uintptr_t base=(uintptr_t)frame->state,address=(uintptr_t)pointer;
    return pointer && address>=base && bytes<=frame->entry->state_bytes &&
        address-base<=frame->entry->state_bytes-bytes;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_aggregate(XrXirCallView *view,
    XrXirOp operation,XrXirType type,XrXirValue *fields,uint32_t count,XrXirValue *output) {
    if (!call_view_move_current(view) || !count || !call_view_slot(view->activation->top,fields) ||
        !call_view_slot(view->activation->top,output) || output->type || output->reserved || output->payload ||
        !call_view_move_span(view->activation->top,fields,(uint64_t)count*sizeof(*fields)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uintptr_t delta=(uintptr_t)output-(uintptr_t)fields;
    if ((uintptr_t)output>=(uintptr_t)fields && delta<(uint64_t)count*sizeof(*fields)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (operation!=XR_XIR_TUPLE_NEW && (operation!=XR_XIR_NULLABLE_SOME || count!=1)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    if (!call_cell_work(call,(uint64_t)count*sizeof(*fields))) return XR_XIR_VALUE_LIMIT;
    CallViewArguments transfer={call,call->top,NULL};
    XrXirViewAccess access={xr_xir_call_admission(view),call_view_argument_source,&transfer};
    call->admitting=true;
    XrXirValueStatus status=operation==XR_XIR_TUPLE_NEW ?
        xr_xir_borrowed_tuple_new_move(type,fields,count,&access,output) :
        xr_xir_borrowed_some_new_move(type,fields,&access,output);
    call->admitting=false;return status;
}
static bool call_view_phi_word(const XrXirViewPhiBatch *batch,uint32_t offset) {
    return batch->frame_bytes>=sizeof(int64_t) && offset<=batch->frame_bytes-sizeof(int64_t) && !(offset%sizeof(int64_t));
}
static XrXirViewPhiTransfer call_view_phi_row(const XrXirViewPhiBatch *batch,uint32_t index) {
    XrXirViewPhiTransfer row;
    memcpy(&row,(const unsigned char *)batch->transfers+(size_t)index*sizeof(row),sizeof(row));return row;
}
static uint32_t call_view_phi_flags(const XrXirViewPhiTransfer *row,const XrXirValueAdmission *admission) {
    return xr_xir_compile_type_arena_borrowed(admission->arena,row->type);
}
static XrXirValueStatus call_view_phi_validate(XrXirCall *call,const XrXirViewPhiBatch *batch,
    const XrXirViewAccess *access) {
    XrXirValueAdmission *admission=access->values;
    for (uint32_t i=0;i<batch->count;++i) {
        XrXirViewPhiTransfer current=call_view_phi_row(batch,i);const XrXirViewPhiTransfer *row=&current;
        if (!call_cell_work(call,5)) return XR_XIR_VALUE_LIMIT;
        if (!call_view_phi_word(batch,row->source) || !call_view_phi_word(batch,row->destination) ||
            !call_view_phi_word(batch,row->scratch) || row->type==XR_XIR_UNIT ||
            xr_xir_scalar_load(batch->frame,row->scratch)) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValue source={(uint32_t)row->type,0,xr_xir_scalar_load(batch->frame,row->source)};
        if (!xr_xir_value_argument(&source,admission->arena,row->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
        uint32_t flags=call_view_phi_flags(row,admission);
        if (flags) {
            XrXirValueStatus status=xr_xir_borrowed_value_admit(&source,row->type,access);
            if (status!=XR_XIR_VALUE_OK) return status;
        }
        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(admission->arena),row->type)) {
            XrXirValue old={(uint32_t)row->type,0,xr_xir_scalar_load(batch->frame,row->destination)};
            if (old.payload && !xr_xir_value_argument(&old,admission->arena,row->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
        for (uint32_t j=0;j<batch->count;++j) {
            if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
            XrXirViewPhiTransfer previous=call_view_phi_row(batch,j);const XrXirViewPhiTransfer *other=&previous;
            if (row->scratch==other->source || row->scratch==other->destination ||
                (i!=j && (row->scratch==other->scratch || row->destination==other->destination)))
                return XR_XIR_VALUE_BAD_ARGUMENT;
            if (i!=j && row->source==other->source && (row->type!=other->type || (flags&XR_XIR_VIEW_MUTABLE)))
                return XR_XIR_VALUE_BAD_ARGUMENT;
            if (j<i && (flags&XR_XIR_VIEW_MUTABLE) && row->type==other->type &&
                source.payload==xr_xir_scalar_load(batch->frame,other->source)) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
    }
    /* Commit and abandonment of old destination owners cannot acquire quota. */
    for (CallViewBinding *binding=call->top->views;binding;binding=binding->next)
        if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
    return call_cell_work(call,(uint64_t)batch->count*6u) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
}
static void call_view_phi_rollback(const XrXirViewPhiBatch *batch,XrXirValueAdmission *admission,uint32_t prepared) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    for (uint32_t i=0;i<prepared;++i) {
        XrXirViewPhiTransfer current=call_view_phi_row(batch,i);const XrXirViewPhiTransfer *row=&current;
        if (call_view_phi_flags(row,admission)&XR_XIR_VIEW_MUTABLE) continue;
        XrXirValue value={(uint32_t)row->type,0,xr_xir_scalar_load(batch->frame,row->scratch)};
        xr_xir_scalar_store(batch->frame,row->scratch,0);
        if (value.payload && xr_xir_type_is_owned(types,row->type)) xr_xir_value_drop(&value);
    }
}
static XrXirValueStatus call_view_phi_prepare(const XrXirViewPhiBatch *batch,XrXirValueAdmission *admission) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    for (uint32_t i=0;i<batch->count;++i) {
        XrXirViewPhiTransfer current=call_view_phi_row(batch,i);const XrXirViewPhiTransfer *row=&current;
        if (call_view_phi_flags(row,admission)&XR_XIR_VIEW_MUTABLE) continue;
        if (xr_xir_type_is_owned(types,row->type)) {
            XrXirValueStatus status=xr_xir_owned_slot_copy(batch->frame,row->scratch,admission->arena,row->type,
                xr_xir_scalar_load(batch->frame,row->source));
            if (status!=XR_XIR_VALUE_OK) {call_view_phi_rollback(batch,admission,i);return status;}
        }
    }
    return XR_XIR_VALUE_OK;
}
static void call_view_phi_commit(const XrXirViewPhiBatch *batch,XrXirValueAdmission *admission) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    for (uint32_t i=0;i<batch->count;++i) {
        XrXirViewPhiTransfer current=call_view_phi_row(batch,i);const XrXirViewPhiTransfer *row=&current;
        if ((call_view_phi_flags(row,admission)&XR_XIR_VIEW_MUTABLE) || !xr_xir_type_is_owned(types,row->type)) {
            xr_xir_scalar_store(batch->frame,row->scratch,xr_xir_scalar_load(batch->frame,row->source));
            if (call_view_phi_flags(row,admission)&XR_XIR_VIEW_MUTABLE) xr_xir_scalar_store(batch->frame,row->source,0);
        }
    }
    for (uint32_t i=0;i<batch->count;++i) {
        XrXirViewPhiTransfer current=call_view_phi_row(batch,i);const XrXirViewPhiTransfer *row=&current;
        XrXirValue value={(uint32_t)row->type,0,xr_xir_scalar_load(batch->frame,row->scratch)};
        xr_xir_scalar_store(batch->frame,row->scratch,0);
        if (xr_xir_type_is_owned(types,row->type)) xr_xir_owned_slot_move(batch->frame,row->destination,&value);
        else xr_xir_scalar_store(batch->frame,row->destination,value.payload);
    }
}
XR_FUNC XrXirValueStatus xr_xir_call_view_phi(XrXirCallView *view,const XrXirViewPhiBatch *input) {
    if (!call_view_move_current(view) || !input) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirViewPhiBatch batch=*input;XrXirCall *call=view->activation;
    if (!batch.count || !batch.transfers || !call_view_move_span(call->top,batch.frame,batch.frame_bytes))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes=(uint64_t)batch.count*sizeof(*batch.transfers);
    uintptr_t frame=(uintptr_t)batch.frame,rows=(uintptr_t)batch.transfers;
    if (bytes>SIZE_MAX || bytes>UINTPTR_MAX-rows || batch.frame_bytes>UINTPTR_MAX-frame ||
        (rows<frame+batch.frame_bytes && frame<rows+bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    CallViewArguments transfer={call,call->top,NULL};
    XrXirViewAccess access={xr_xir_call_admission(view),call_view_argument_source,&transfer};
    call->admitting=true;
    XrXirValueStatus status=call_view_phi_validate(call,&batch,&access);
    if (status==XR_XIR_VALUE_OK) status=call_view_phi_prepare(&batch,access.values);
    if (status==XR_XIR_VALUE_OK) {
        call_view_phi_commit(&batch,access.values);
        XrXirValueStatus collected=call_view_collect(call,false);
        XR_CHECK(collected==XR_XIR_VALUE_OK,"prepaid PHI binding retirement cannot fail");
    }
    call->admitting=false;return status;
}
#include "xxir_call_view_end_words.inc.c"
