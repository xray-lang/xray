/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_view_aggregate.inc.c - Owned scratch staging with complete rollback
 */
static uint32_t emit_view_field_operand(const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirTypeNode *tuple,uint32_t field,uint32_t *payload) {
    if (!tuple) return op->args[0];
    return tuple->parameters[field].type==XR_XIR_UNIT?UINT32_MAX:function->operands[op->args[0]+(*payload)++];
}
static void emit_view_aggregate(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination) {
    const XrXirTypeNode *tuple=op->op==XR_XIR_TUPLE_NEW?xr_xir_tuple_signature(buffer->types,op->type):NULL;
    uint32_t count=tuple?tuple->parameter_count:1,payload=0;
    append(buffer," { XrXirValueStatus status=XR_XIR_VALUE_OK;\n"
        " for (uint32_t i=0;i<%uu;++i) state->arguments[i]=(XrXirValue){0};\n",count+1u);
    for (uint32_t i=0;i<count && ((buffer)->status == XR_XIR_OK);++i) {
        uint32_t id=emit_view_field_operand(function,op,tuple,i,&payload);if (id==UINT32_MAX) continue;
        append(buffer," if (status==XR_XIR_VALUE_OK) { XrXirValue source=");emit_value(buffer,function,layout,id);
        if (emit_borrowed_flags(buffer,xr_xir_operand_type(function,id))&XR_XIR_VIEW_MUTABLE)
            append(buffer,"; if (!xr_xir_value_argument(&source,view->arena,(XrXirType)source.type)) status=XR_XIR_VALUE_BAD_ARGUMENT;\n"
                " else { state->arguments[%u]=source; xr_xir_scalar_store(state->frame,%uu,0); } }\n",i,layout->offsets[id]);
        else append(buffer,"; status=xr_xir_value_copy(&source,state->arguments+%u); }\n",i);
    }
    append(buffer," if (status==XR_XIR_VALUE_OK) status=xr_xir_call_view_aggregate(view,(XrXirOp)%uu,"
        "(XrXirType)%uu,state->arguments,%uu,state->arguments+%u);\n",(uint32_t)op->op,(uint32_t)op->type,count,count);
    payload=0;
    for (uint32_t i=0;i<count && ((buffer)->status == XR_XIR_OK);++i) {
        uint32_t id=emit_view_field_operand(function,op,tuple,i,&payload);if (id==UINT32_MAX) continue;
        if (emit_borrowed_flags(buffer,xr_xir_operand_type(function,id))&XR_XIR_VIEW_MUTABLE)
            append(buffer," if (status!=XR_XIR_VALUE_OK && state->arguments[%u].type)\n"
                " xr_xir_owned_slot_move(state->frame,%uu,state->arguments+%u);\n"
                " else xr_xir_value_drop(state->arguments+%u);\n",i,layout->offsets[id],i,i);
        else append(buffer," xr_xir_value_drop(state->arguments+%u);\n",i);
    }
    append(buffer," if (status==XR_XIR_VALUE_OK) xr_xir_owned_slot_move(state->frame,%uu,state->arguments+%u);\n"
        " xr_xir_value_drop(state->arguments+%u);\n",destination,count,count);
    emit_view_move_status(buffer);
    append(buffer," }\n return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};\n");
}
