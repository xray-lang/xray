/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_select.inc.c - Prepared typed values before a joint receive commit
 *
 * KEY CONCEPT:
 *   The Call owns the parked frontier. Provider calls and tuple allocation
 *   precede the winner commit; publication after that commit cannot fail.
 */
static XrXirCallStatus call_select_clock(XrXirCall *call,bool needed,uint64_t *now) {
    *now=0;if(!needed)return XR_XIR_CALL_READY;
    XrXirCallStatus status=channel_timing_clock(call,now);
    if(status!=XR_XIR_CALL_READY)return status;
    if(*now<call->channel_select_clock)return XR_XIR_CALL_HOST_ERROR;
    call->channel_select_clock=*now;return XR_XIR_CALL_READY;
}
static XrXirCallStatus call_select_deadline_poll(XrXirCall *call) {
    XrXirChannelSelectDeadline deadline={0};
    if(!call->channel_select||!xr_xir_channel_select_deadline(call->channel_select,&deadline))
        return XR_XIR_CALL_BAD_STATE;
    if(!deadline.enabled)return XR_XIR_CALL_READY;
    uint64_t now=0;XrXirCallStatus status=call_select_clock(call,true,&now);
    if(status!=XR_XIR_CALL_READY)return status;
    if(now<deadline.start_ns||now<deadline.last_ns)return XR_XIR_CALL_HOST_ERROR;
    call->channel_select_due=now-deadline.start_ns>=deadline.duration_ns;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus call_select_shape(XrXirValueAdmission *admission,
    const XrXirChannelSelectCallRequest *request,const XrXirValue *output) {
    if(!request->channels||!request->count||request->count>=65536u)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if(request->has_default&&request->has_timeout)return XR_XIR_CALL_UNSUPPORTED;
    if(!request->has_timeout&&request->timeout_ms)return XR_XIR_CALL_BAD_ARGUMENT;
    if(request->timeout_ms<0||(uint64_t)request->timeout_ms>XR_XIR_TIMER_MAX_MS)
        return XR_XIR_CALL_NUMERIC_RANGE;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    const XrXirTypeNode *tuple=xr_xir_type_node(types,request->result_type);
    if(!tuple||tuple->kind!=XR_XIR_TYPE_TUPLE||!tuple->parameters||
        tuple->parameter_count!=request->count+1||tuple->parameters[0].type!=XR_XIR_I64||
        tuple->parameters[0].mode!=XR_PARAM_READ)return XR_XIR_CALL_BAD_ARGUMENT;
    for(uint32_t a=0;a<request->count;++a) {
        if(!channel_call_work(admission))return XR_XIR_CALL_LIMIT;
        const XrXirValue *channel=request->channels+a;
        XrXirType type=(XrXirType)channel->type;
        const XrXirTypeNode *field=xr_xir_type_node(types,tuple->parameters[a+1].type);
        if(output==channel||!xr_xir_type_is_channel(types,type)||
            !xr_xir_value_argument(channel,admission->arena,type)||!field||
            field->kind!=XR_XIR_TYPE_NULLABLE||field->element!=xr_xir_channel_element(types,type)||
            tuple->parameters[a+1].mode!=XR_PARAM_READ)return XR_XIR_CALL_BAD_ARGUMENT;
    }
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus call_select_result(XrXirCall *call,XrXirChannelSelectKind kind,
    uint32_t arm,const XrXirValue *payload,XrXirValue *output) {
    XrXirValueAdmission *admission=&call->config.admission;
    const XrXirTypeNode *tuple=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),call->channel_select_type);
    if(!tuple||tuple->kind!=XR_XIR_TYPE_TUPLE||!tuple->parameters||!tuple->parameter_count)
        return XR_XIR_CALL_BAD_STATE;
    uint32_t count=tuple->parameter_count;
    if(kind==XR_XIR_CHANNEL_SELECT_RECEIVE&&arm>=count-1)return XR_XIR_CALL_BAD_STATE;
    XrXirValueStatus value=XR_XIR_VALUE_OK;
    uint64_t bytes=(uint64_t)count*sizeof(XrXirValue);
    XrXirValue *fields=xr_xir_domain_metadata_allocate(admission->domain,bytes,&value);
    if(!fields)return channel_call_value_status(value);
    memset(fields,0,(size_t)bytes);
    fields[0]=(XrXirValue){XR_XIR_I64,0,kind==XR_XIR_CHANNEL_SELECT_RECEIVE?arm:count-1};
    for(uint32_t a=1;value==XR_XIR_VALUE_OK&&a<count;++a)
        value=xr_xir_nullable_new(tuple->parameters[a].type,
            kind==XR_XIR_CHANNEL_SELECT_RECEIVE&&a==arm+1?payload:NULL,admission,&fields[a]);
    if(value==XR_XIR_VALUE_OK)value=xr_xir_tuple_new(call->channel_select_type,fields,count,admission,output);
    for(uint32_t a=0;a<count;++a)xr_xir_value_drop(&fields[a]);
    xr_xir_domain_metadata_deallocate(admission->domain,fields,bytes);
    return channel_call_value_status(value);
}
XR_FUNC XrXirCallStatus xr_xir_call_select_step(XrXirCallView *view,
    XrXirChannelSelectCallState *state,const XrXirChannelSelectCallRequest *request,XrXirValue *output) {
    if(!state||!request||!output||!boundary_value(*output,XR_XIR_UNIT)||state->waiting>2)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!admission)return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call=view->activation;
    if(cleanup_active(call))return XR_XIR_CALL_DEFER_ASYNC;
    XrXirCallStatus status=xr_xir_call_execution_status(view);
    if(status!=XR_XIR_CALL_READY) {if(call->channel_select)release_channel_wait(call);return status;}
    status=call_select_shape(admission,request,output);
    if(status!=XR_XIR_CALL_READY)return status;
    if(call->channel_watch||call->channel_send||call->channel_recv||call->channel_timing.active||
        (call->channel_select&&(call->channel_epoch!=call->top->epoch||
            call->channel_select_type!=request->result_type||!state->waiting)))return XR_XIR_CALL_BAD_STATE;
    if(state->waiting==2) {
        XrXirValue wake={0};status=xr_xir_call_take_inbox(view,XR_XIR_UNIT,&wake);
        if(status!=XR_XIR_CALL_READY)return status;
        state->waiting=1;
    }
    uint64_t now=0;status=call_select_clock(call,request->has_timeout,&now);
    if(status!=XR_XIR_CALL_READY)goto failed;
    if(!call->channel_select) {
        if(state->waiting)return XR_XIR_CALL_BAD_STATE;
        const XrXirChannelSelectRequest begin={request->channels,request->count,request->has_default,
            request->has_timeout,request->timeout_ms,now};
        status=channel_call_value_status(xr_xir_channel_select_begin(&begin,admission,&call->channel_select));
        if(status!=XR_XIR_CALL_READY)goto failed;
        call->channel_select_type=request->result_type;call->channel_epoch=call->top->epoch;
        state->waiting=1;
    }
    uint64_t entropy=0;
    xr_random_bytes((unsigned char *)&entropy,sizeof(entropy));
    XrXirChannelSelectState observed=XR_XIR_CHANNEL_SELECT_RETRY;
    status=channel_call_value_status(xr_xir_channel_select_probe(call->channel_select,now,entropy,admission,&observed));
    if(status!=XR_XIR_CALL_READY)goto failed;
    call->channel_select_due=false;
    if(observed==XR_XIR_CHANNEL_SELECT_RETRY)return XR_XIR_CALL_BUSY;
    if(observed==XR_XIR_CHANNEL_SELECT_PENDING) {
        if(call->next_wake==UINT64_MAX){status=XR_XIR_CALL_BAD_STATE;goto failed;}
        state->waiting=2;call->channel_select_pending=true;return XR_XIR_CALL_SUSPENDED;
    }
    XrXirChannelSelectKind kind;uint32_t arm=0;const XrXirValue *payload=NULL;
    if(observed!=XR_XIR_CHANNEL_SELECT_CANDIDATE||
        !xr_xir_channel_select_candidate(call->channel_select,&kind,&arm,&payload)) {
        status=XR_XIR_CALL_BAD_STATE;goto failed;
    }
    XrXirValue prepared={0};status=call_select_result(call,kind,arm,payload,&prepared);
    if(status==XR_XIR_CALL_READY)status=call_select_clock(call,request->has_timeout,&now);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY)
        status=channel_call_value_status(xr_xir_channel_select_commit(call->channel_select,now,admission,&observed));
    if(status!=XR_XIR_CALL_READY) {xr_xir_value_drop(&prepared);goto failed;}
    if(observed==XR_XIR_CHANNEL_SELECT_RETRY) {xr_xir_value_drop(&prepared);return XR_XIR_CALL_BUSY;}
    XrXirValue consumed={0};XrXirChannelSelectKind committed;uint32_t selected=0;
    XR_CHECK(observed==XR_XIR_CHANNEL_SELECT_COMMITTED&&
        xr_xir_channel_select_take(call->channel_select,&committed,&selected,&consumed)&&
        committed==kind&&selected==arm,"one committed winner matches the prepared typed tuple");
    xr_xir_value_drop(&consumed);release_channel_wait(call);*state=(XrXirChannelSelectCallState){0};
    *output=prepared;return XR_XIR_CALL_READY;
failed:
    release_channel_wait(call);*state=(XrXirChannelSelectCallState){0};return status;
}
