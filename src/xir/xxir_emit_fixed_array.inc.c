/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_fixed_array.inc.c - Portable fixed byte recipes over shared values
 *
 * KEY CONCEPT:
 *   Native code uses the VM's exact payload, admission and bounds helpers.
 */
static void emit_fixed_array_step(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination,uint32_t index) {
    EMIT_LITERAL(buffer," { XrXirValue value = {0}; XrXirValueStatus status; XrXirFaultDetail fault = {0};\n");
    if(op->op==XR_XIR_FIXED_ARRAY_NEW) {
        for(uint32_t p=0;p<op->args[1] && buffer->status==XR_XIR_OK;++p) {
            append(buffer," state->arguments[%u] = ",p);
            emit_value(buffer,function,layout,function->operands[op->args[0]+p]);EMIT_LITERAL(buffer,";\n");
        }
        append(buffer," status = xr_xir_fixed_array_new((XrXirType)%uu, state->arguments, %uu, xr_xir_call_admission(view), &value);\n",
            (uint32_t)op->type,op->args[1]);
    } else {
        EMIT_LITERAL(buffer," XrXirValue receiver = ");emit_value(buffer,function,layout,op->args[0]);
        append(buffer,";\n status = xr_xir_value_admit(&receiver, (XrXirType)%uu, xr_xir_call_admission(view));\n",
            (uint32_t)xr_xir_operand_type(function,op->args[0]));
        if(op->op==XR_XIR_FIXED_ARRAY_GET) {
            EMIT_LITERAL(buffer," if (status == XR_XIR_VALUE_OK) status = xr_xir_fixed_array_get(&receiver, ");
            append(buffer,"xr_xir_scalar_load(state->frame,%uu)",layout->offsets[op->args[1]]);
            EMIT_LITERAL(buffer,", &value, &fault);\n");
        } else EMIT_LITERAL(buffer," if (status == XR_XIR_VALUE_OK) { int64_t count = 0;\n"
            " status = xr_xir_fixed_array_count(&receiver, &count);\n"
            " if (status == XR_XIR_VALUE_OK) value = (XrXirValue){XR_XIR_I64,0,count}; }\n");
    }
    EMIT_LITERAL(buffer," if (status == XR_XIR_VALUE_BOUNDS) ");
    emit_fault_return(buffer,function,layout,index,"(XrXirAction){.kind=XR_XIR_ACTION_FAULT,"
        ".value={XR_XIR_I64,0,XR_XIR_CALL_BOUNDS},.panic={.detail=fault}}");
    EMIT_LITERAL(buffer,"\n if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
        " status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
        " status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n");
    append(buffer,op->op==XR_XIR_FIXED_ARRAY_NEW?
        " xr_xir_owned_slot_move(state->frame, %uu, &value);\n":" xr_xir_scalar_store(state->frame, %uu, value.payload);\n",destination);
    EMIT_LITERAL(buffer," }\n return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};\n");
}
