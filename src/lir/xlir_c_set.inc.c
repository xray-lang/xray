/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_set.inc.c - Portable C11 for the closed I64 Set family
 *
 * KEY CONCEPT:
 *   Typed operations retain and retire actual Set owners and local bindings.
 */
static bool c_set_point(LirC *c,const XrLirInstructionView *v) {
    return (v->op>=XR_LIR_SET_NEW && v->op<=XR_LIR_SET_REF_WRITE) ||
        (v->op==XR_LIR_CELL_NEW && v->type==XR_LIR_SET_I64_CELL) ||
        (v->op==XR_LIR_CELL_READ && v->type==XR_LIR_SET_I64) ||
        (v->op==XR_LIR_CELL_WRITE && c_value_type(c,v->operands[0])==XR_LIR_SET_I64_CELL);
}
static void c_set_instruction(LirC *c,const XrLirInstructionView *v,uint32_t source) {
    XrLirType receiver=v->operand_count?c_value_type(c,v->operands[0]):XR_LIR_TYPE_INVALID;
    if(v->op==XR_LIR_SET_NEW) {c_format(c,"    v%" PRIu32 " = xrt_set_i64_new(NULL, 0, UINT32_C(%" PRIu32 "));\n",v->result,source);return;}
    if(v->op==XR_LIR_SET_DROP || v->op==XR_LIR_SET_CELL_DROP) {c_text(c,"    xrt_set_drop(");if(v->op==XR_LIR_SET_DROP)c_text(c,"&");c_value(c,v->operands[0]);c_text(c,");\n");return;}
    if(v->op==XR_LIR_CELL_NEW) {
        c_format(c,"    XrtSet *xrl_cell%" PRIu32 " = xrt_set_retain(",v->result);c_value(c,v->operands[0]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n",source,v->result,v->result);return;
    }
    const char *call=NULL;
    switch(v->op) {
    case XR_LIR_SET_RETAIN:call="xrt_set_retain";break;
    case XR_LIR_SET_LEN:call="xrt_set_length";break;
    case XR_LIR_SET_CONTAINS:call="xrt_set_i64_contains";break;
    case XR_LIR_SET_ADD:call=receiver==XR_LIR_SET_I64_REF?"xrt_set_ref_i64_add":"xrt_set_i64_add";break;
    case XR_LIR_SET_REMOVE:call=receiver==XR_LIR_SET_I64_REF?"xrt_set_ref_i64_remove":"xrt_set_i64_remove";break;
    case XR_LIR_SET_CLEAR:call=receiver==XR_LIR_SET_I64_REF?"xrt_set_ref_clear":"xrt_set_clear";break;
    case XR_LIR_SET_REF_READ:call="xrt_set_ref_read";break;
    case XR_LIR_SET_REF_WRITE:call="xrt_set_ref_write";break;
    case XR_LIR_CELL_READ:call="xrt_set_retain";break;
    case XR_LIR_CELL_WRITE:call="xrt_set_assign";break;
    case XR_LIR_SET_BORROW:call=receiver==XR_LIR_STATE_REF?"xrt_set_ref_state":"xrt_set_ref_local";break;
    default:c_error(c,XR_LIR_C_UNSUPPORTED,"Set operation has no native role");return;
    }
    if(v->type!=XR_LIR_UNIT)c_format(c,"    v%" PRIu32 " = %s(",v->result,call);else c_format(c,"    %s(",call);
    for(uint32_t p=0;p<v->operand_count;++p) {
        if(p)c_text(c,", ");
        if(!p && v->op==XR_LIR_CELL_READ)c_text(c,"*");c_value(c,v->operands[p]);
    }
    if(v->operand_count)c_text(c,", ");c_format(c,"UINT32_C(%" PRIu32 "));\n",source);
}
