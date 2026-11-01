/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_take.inc.c - Prepared child ownership without copying caller words
 *
 * KEY CONCEPT:
 *   A staged argument remains owned by the caller until every child admission
 *   succeeds. Only the final no-fail commit clears its real compact-frame word.
 */
static bool call_take_parameter(const XirCallTakeArguments *take,uint32_t parameter) {
    if(!take||!take->caller||parameter>=take->count)return false;
    if(take->signature)return take->signature->parameters[parameter].mode==XR_PARAM_MOVE;
    return xr_xir_declared_parameter_mode(NULL,take->callee->parameters[parameter],
        take->callee->consume_parameters,take->callee->consume_parameter_count,parameter)==XR_PARAM_MOVE;
}
static void *call_take_word(const XirCallTakeArguments *take,uint32_t parameter) {
    const XrXirInstruction *op=&take->caller->instructions[take->instruction];
    uint32_t value=take->caller->operands[op->args[0]+parameter];
    return (unsigned char *)take->frame+take->layout->offsets[value];
}
XR_FUNC XrXirCallStatus xir_call_arguments_stage(XrXirCallView *view,const XirCallTakeArguments *take) {
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->activation->admitting||
        !take||!take->caller||!take->layout||(!take->callee==!take->signature)||
        take->instruction>=take->caller->instruction_count||
        (take->count&&!take->arguments)||!xir_call_frame_span(view,take->frame,take->layout->frame_bytes))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=&take->caller->instructions[take->instruction];
    if(op->args[1]!=take->count||op->args[0]>take->caller->operand_count||
        take->count>take->caller->operand_count-op->args[0]||
        (take->signature?take->signature->parameter_count:take->callee->parameter_count)!=take->count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    bool needed=false;
    for(uint32_t a=0;a<take->count;++a) {
        if(!call_cell_work(view->activation,1))return XR_XIR_CALL_LIMIT;
        if(!call_take_parameter(take,a))continue;
        needed=true;uint32_t value=take->caller->operands[op->args[0]+a];
        if(value>=take->caller->parameter_count+take->caller->instruction_count||
            take->layout->offsets[value]>take->layout->frame_bytes||
            sizeof(int64_t)>take->layout->frame_bytes-take->layout->offsets[value])return XR_XIR_CALL_BAD_ARGUMENT;
        int64_t payload=0;memcpy(&payload,call_take_word(take,a),sizeof(payload));
        if(payload!=take->arguments[a].payload)return XR_XIR_CALL_BAD_ARGUMENT;
        for(uint32_t b=0;b<a;++b) {
            if(!call_cell_work(view->activation,1))return XR_XIR_CALL_LIMIT;
            if(call_take_parameter(take,b)&&call_take_word(take,b)==call_take_word(take,a))
                return XR_XIR_CALL_BAD_ARGUMENT;
        }
    }
    XrXirCallStatus status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY&&needed)view->activation->top->take=*take;
    return status;
}
static bool call_take_pending(const CallFrame *frame,uint32_t parameter) {
    return frame->take_source&&parameter>=frame->take_captures&&
        call_take_parameter(frame->take_source,parameter-frame->take_captures);
}
static XrXirCallStatus call_take_recheck(XrXirCall *call,CallFrame *frame) {
    const XirCallTakeArguments *take=frame->take_source;
    if(!take)return XR_XIR_CALL_READY;
    if(!call->active_view||frame->parent!=call->top||take!=&call->top->take||
        frame->entry!=&frame->metadata->entries[take->target]||
        frame->entry->parameter_count-frame->take_captures!=take->count)
        return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=frame->take_captures;p<frame->entry->parameter_count;++p) {
        if(!call_cell_work(call,1))return XR_XIR_CALL_LIMIT;
        if(!call_take_pending(frame,p))continue;
        int64_t payload=0;memcpy(&payload,call_take_word(take,p-frame->take_captures),sizeof(payload));
        if(payload!=frame->arguments[p].payload)return XR_XIR_CALL_BAD_ARGUMENT;
    }
    return xr_xir_call_execution_status(call->active_view);
}
static void call_take_commit(CallFrame *frame) {
    if(!frame->take_source)return;
    for(uint32_t p=frame->take_captures;p<frame->entry->parameter_count;++p)
        if(call_take_pending(frame,p))memset(call_take_word(frame->take_source,p-frame->take_captures),0,sizeof(int64_t));
    frame->take_source=NULL;
}
XR_FUNC XrXirValueStatus xir_call_owned_argument_take(XrXirCallView *view,uint32_t parameter,XrXirValue *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!admission||view->activation->admitting||view->phase!=XR_XIR_CALL_NORMAL||
        parameter>=view->argument_count||!output||!xir_call_empty_destination(output)||
        !call_view_slot(view->activation->top,output))return XR_XIR_VALUE_BAD_ARGUMENT;
    CallFrame *frame=view->activation->top;XrXirType type=frame->entry->parameters[parameter];
    if(xr_xir_type_is_cell(xr_xir_compile_type_arena_types(view->arena),type)||
        xr_xir_compile_type_arena_borrowed(view->arena,type)||
        !xr_xir_value_argument(&frame->arguments[parameter],view->arena,type))return XR_XIR_VALUE_BAD_ARGUMENT;
    if(!call_cell_work(view->activation,1))return XR_XIR_VALUE_LIMIT;
    if(xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY)return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=frame->arguments[parameter];frame->arguments[parameter]=(XrXirValue){0};return XR_XIR_VALUE_OK;
}
