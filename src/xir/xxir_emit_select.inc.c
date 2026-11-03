/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_select.inc.c - Portable joint-wait statements
 *
 * KEY CONCEPT:
 *   The generated adapter retries the same sealed instruction. A successful
 *   receive publishes its already prepared tuple without fallible work.
 */
static void emit_select_step(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination,uint32_t index) {
    uint32_t count=op->args[1]-(op->immediate==2?1u:0u);
    append(buffer," state->pc=%uu;\n { XrXirValue output={0};\n",index);
    for(uint32_t a=0;a<count&&buffer->status==XR_XIR_OK;++a) {
        append(buffer," state->arguments[%u]=",a);
        emit_value(buffer,function,layout,function->operands[op->args[0]+a]);append(buffer,";\n");
    }
    append(buffer," XrXirCallStatus status=xr_xir_instance_channel_select_at(view,%uu,state->frame,&state->select,state->arguments,&output);\n"
        " if(status==XR_XIR_CALL_BUSY||status==XR_XIR_CALL_CANCELLED||status==XR_XIR_CALL_SUSPENDED||status==XR_XIR_CALL_DEFER_ASYNC)\n"
        " return (XrXirAction){.kind=status==XR_XIR_CALL_SUSPENDED||status==XR_XIR_CALL_DEFER_ASYNC?XR_XIR_ACTION_AWAIT_CHANNEL:XR_XIR_ACTION_CONTINUE};\n"
        " if(status==XR_XIR_CALL_RUNTIME_PANIC) ",index);
    emit_fault_return(buffer,function,layout,index,
        "(XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_RUNTIME_PANIC},"
        ".panic={.detail={XR_XIR_PANIC_CHANNEL_CLOSED,0,0,0}}}");
    append(buffer,"\n if(status!=XR_XIR_CALL_READY) ");
    emit_fault_return(buffer,function,layout,index,
        "xr_xir_call_fault(status==XR_XIR_CALL_OOM?XR_XIR_RUN_OUT_OF_MEMORY:"
        "status==XR_XIR_CALL_LIMIT?XR_XIR_RUN_FRAME_LIMIT:status==XR_XIR_CALL_HOST_ERROR?XR_XIR_RUN_HOST_ERROR:"
        "status==XR_XIR_CALL_NUMERIC_RANGE?XR_XIR_RUN_NUMERIC_RANGE:"
        "status==XR_XIR_CALL_UNSUPPORTED?XR_XIR_RUN_UNSUPPORTED:XR_XIR_RUN_BAD_ARTIFACT)");
    append(buffer,"\n xr_xir_owned_slot_move(state->frame,%uu,&output);state->pc=%uu;\n }\n"
        " return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};\n",destination,index+1);
}
