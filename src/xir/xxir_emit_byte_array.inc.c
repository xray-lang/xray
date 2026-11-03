/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_byte_array.inc.c - Portable byte candidate calls
 *
 * KEY CONCEPT:
 *   Generated C calls the same active-instance transaction as the interpreter.
 */
static void emit_byte_array_step(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination,uint32_t index) {
    bool repeat=op->op==XR_XIR_BYTE_ARRAY_REPEAT;
    const uint32_t *ids=repeat?function->operands+op->args[0]:op->args;
    append(buffer," { XrXirValue value={0}, array=");emit_value(buffer,function,layout,ids[0]);
    append(buffer,"; XrXirFaultDetail fault={0}; XrXirCallStatus status;\n");
    if (repeat) append(buffer," status=xr_xir_instance_array_byte_repeat(view,&array,"
        "xr_xir_scalar_load(state->frame,%uu),xr_xir_scalar_load(state->frame,%uu),&value,&fault);\n",
        layout->offsets[ids[1]],layout->offsets[ids[2]]);
    else {
        append(buffer," XrXirValue snapshot=");emit_value(buffer,function,layout,ids[1]);
        append(buffer,"; status=xr_xir_instance_array_byte_append(view,&array,&snapshot,&value);\n");
    }
    append(buffer," if(status!=XR_XIR_CALL_READY) ");
    emit_fault_return(buffer,function,layout,index,
        "(XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status},.panic={.detail=fault}}");
    append(buffer,"\n xr_xir_owned_slot_move(state->frame,%uu,&value); }\n"
        " return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};\n",destination);
}
