/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_select.inc.c - Actual sealed receive operands for one step
 *
 * KEY CONCEPT:
 *   Only the current instruction supplies the arm graph and result type.
 *   Its trusted compiler adapter lends the frame; no frame address survives.
 */
XR_FUNC XrXirCallStatus xr_xir_instance_channel_select_at(XrXirCallView *view,
    uint32_t instruction,void *frame,XrXirChannelSelectCallState *state,
    const XrXirValue *channels,XrXirValue *output) {
    if(!instance_view_current(view)||!channels)return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    const XrXirProgramPermissions *permissions=instance?instance->program->permissions:NULL;
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    if(!permissions||!permissions->input||!permissions->input->artifact||!permissions->layouts||
        entry>=permissions->input->artifact->module.function_count)return XR_XIR_CALL_BAD_STATE;
    const XrXirModule *module=&permissions->input->artifact->module;
    const XrXirFunction *caller=module->functions+entry;
    const XrXirFunctionLayout *layout=permissions->layouts+entry;
    if(instruction>=caller->instruction_count)return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=caller->instructions+instruction;
    if(xir_channel_select_shape(module,caller,op,NULL)!=XR_XIR_OK||
        !xir_call_frame_span(view,frame,layout->frame_bytes))return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!admission)return XR_XIR_CALL_BAD_STATE;
    uint32_t count=op->args[1]-(op->immediate==2?1u:0u);int64_t timeout=0;
    for(uint32_t a=0;a<op->args[1];++a) {
        if(!admission->work||!xr_xir_domain_work(admission->domain,1))return XR_XIR_CALL_LIMIT;
        --admission->work;
        uint32_t value=caller->operands[op->args[0]+a],offset=layout->offsets[value];int64_t payload=0;
        if(offset>layout->frame_bytes||sizeof(payload)>layout->frame_bytes-offset)
            return XR_XIR_CALL_BAD_ARGUMENT;
        memcpy(&payload,(unsigned char *)frame+offset,sizeof(payload));
        if(a==count)timeout=payload;
        else if(channels[a].type!=(uint32_t)xr_xir_operand_type(caller,value)||
            channels[a].reserved||channels[a].payload!=payload)return XR_XIR_CALL_BAD_ARGUMENT;
    }
    const XrXirChannelSelectCallRequest request={channels,count,op->type,
        op->immediate==1,op->immediate==2,timeout};
    return xr_xir_call_select_step(view,state,&request,output);
}
