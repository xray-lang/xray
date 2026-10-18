/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_view.inc.c - Portable C11 over the common scoped view operations
 *
 * KEY CONCEPT:
 *   Full owned scratch and compact frame payloads transfer exactly one owner.
 */
static void emit_view_return(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout) {
    uint32_t offset=layout->offsets[op->args[0]];
    append(buffer," { state->arguments[0]=(XrXirValue){%uu,0,xr_xir_scalar_load(state->frame,%uu)};\n"
        " xr_xir_scalar_store(state->frame,%uu,0);\n"
        " XrXirAction action={0}; XrXirCallStatus status=xr_xir_call_return_move(view,state->arguments,&action);\n"
        " if (status==XR_XIR_CALL_READY) return action;\n"
        " xr_xir_owned_slot_move(state->frame,%uu,state->arguments);\n"
        " return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0}; }\n",
        (uint32_t)function->result,offset,offset,offset);
}
static void emit_view_step(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination,uint32_t index) {
    bool group=xr_xir_op_uses_operand_table(op->op),end=op->op==XR_XIR_VIEW_END;
    const uint32_t *args=group ? function->operands+op->args[0] : op->args;
    append(buffer,"        { XrXirValue root = ");emit_value(buffer,function,layout,args[0]);
    append(buffer,"; XrXirFaultDetail fault = {0};\n"
        "        state->arguments[0] = state->arguments[1] = (XrXirValue){0};\n"
        "        XrXirCallViewOperation request = {.operation=(XrXirOp)%uu, .type=(XrXirType)%uu, .root=&root};\n"
        "        XrXirValueStatus status = XR_XIR_VALUE_OK;\n",(uint32_t)op->op,(uint32_t)op->type);
    if (op->op==XR_XIR_VIEW_NEW)
        append(buffer,"        status = xr_xir_value_copy(&root, state->arguments);\n"
            "        request.root = state->arguments;\n");
    else if (end)
        append(buffer,"        state->arguments[0] = root;\n"
            "        xr_xir_scalar_store(state->frame, %uu, 0);\n"
            "        request.root = NULL; request.owned = state->arguments;\n",layout->offsets[args[0]]);
    if (group || op->op==XR_XIR_VIEW_GET)
        append(buffer,"        request.start = xr_xir_scalar_load(state->frame, %uu);\n",layout->offsets[args[1]]);
    if (op->op==XR_XIR_VIEW_SET) {
        append(buffer,"        XrXirValue element = ");emit_value(buffer,function,layout,args[2]);
        append(buffer,"; request.element = &element;\n");
    } else if (group)
        append(buffer,"        request.end = xr_xir_scalar_load(state->frame, %uu); request.to_end = %s;\n",
            layout->offsets[args[2]],op->immediate ? "true" : "false");
    append(buffer,"        if (status == XR_XIR_VALUE_OK) status = xr_xir_call_view_operation(view, &request, state->arguments + 1, &fault);\n");
    if (end)
        append(buffer,"        if (status != XR_XIR_VALUE_OK) xr_xir_owned_slot_move(state->frame, %uu, state->arguments);\n"
            "        else xr_xir_value_drop(state->arguments);\n",layout->offsets[args[0]]);
    else append(buffer,"        xr_xir_value_drop(state->arguments);\n");
    if (op->type!=XR_XIR_UNIT) {
        if (xr_xir_type_is_owned(buffer->types,op->type))
            append(buffer,"        if (status == XR_XIR_VALUE_OK) xr_xir_owned_slot_move(state->frame, %uu, state->arguments + 1);\n",destination);
        else append(buffer,"        if (status == XR_XIR_VALUE_OK) { xr_xir_scalar_store(state->frame, %uu, state->arguments[1].payload); state->arguments[1] = (XrXirValue){0}; }\n",destination);
    }
    append(buffer,"        xr_xir_value_drop(state->arguments + 1);\n        if (status != XR_XIR_VALUE_OK) ");
    emit_fault_return(buffer,function,layout,index,"xr_xir_call_view_operation_fault(status, fault)");
    append(buffer,"\n        }\n        return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
static void emit_view_cleanup(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t index) {
    if (op->args[1]) {
        append(buffer," { static const XrXirViewEndWord words[]={\n");
        for (uint32_t i=0;i<op->args[1] && emit_work(buffer,1);++i) {
            uint32_t value=function->operands[op->args[0]+i];
            append(buffer," {(XrXirType)%uu,%uu},\n",(uint32_t)xr_xir_operand_type(function,value),layout->offsets[value]);
        }
        append(buffer," }; XrXirViewEndBatch batch={state->frame,%uu,words,%uu};\n"
            " XrXirValueStatus status=xr_xir_call_view_end_words(view,&batch);\n"
            " if (status!=XR_XIR_VALUE_OK) ",layout->frame_bytes,op->args[1]);
        emit_fault_return(buffer,function,layout,index,"xr_xir_call_view_operation_fault(status,(XrXirFaultDetail){0})");
        append(buffer,"\n }\n");
    }
    append(buffer," return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};\n");
}
