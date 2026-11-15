/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_set.inc.c - Typed I64 Set operand directory
 *
 * KEY CONCEPT:
 *   Set identity and ownership stay distinct from every other managed family.
 */
static bool rbc_lir_i64set_point(RbcLirCompiler *c,const XrLirInstructionView *i) {
    if(i->op>=XR_LIR_SET_NEW && i->op<=XR_LIR_SET_REF_WRITE)return true;
    if(i->op==XR_LIR_CELL_NEW)return i->type==XR_LIR_SET_I64_CELL;
    if(i->op==XR_LIR_CELL_READ || i->op==XR_LIR_STATE_READ)return i->type==XR_LIR_SET_I64;
    if(i->op==XR_LIR_CELL_WRITE || i->op==XR_LIR_STATE_WRITE) {
        XrLirType type=XR_LIR_TYPE_INVALID;uint32_t operand=i->op==XR_LIR_STATE_WRITE?1:0;
        return xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,i->operands[operand]},&type)==XR_LIR_OK &&
            type==(XrLirType)(i->op==XR_LIR_STATE_WRITE?XR_LIR_SET_I64:XR_LIR_SET_I64_CELL);
    }
    return false;
}
static bool rbc_lir_i64set_emit(RbcLirCompiler *c,const XrLirInstructionView *i) {
    XrRbcArrayOperation operation;
    switch(i->op) {
    case XR_LIR_SET_NEW:operation=XR_RBC_ARRAY_NEW;break;
    case XR_LIR_SET_LEN:operation=XR_RBC_ARRAY_LEN;break;
    case XR_LIR_SET_CONTAINS:operation=XR_RBC_SET_CONTAINS;break;
    case XR_LIR_SET_ADD:operation=XR_RBC_SET_ADD;break;
    case XR_LIR_SET_REMOVE:operation=XR_RBC_SET_REMOVE;break;
    case XR_LIR_SET_CLEAR:operation=XR_RBC_SET_CLEAR;break;
    case XR_LIR_SET_RETAIN:operation=XR_RBC_ARRAY_RETAIN;break;
    case XR_LIR_SET_DROP:operation=XR_RBC_ARRAY_DROP;break;
    case XR_LIR_SET_CELL_DROP:operation=XR_RBC_ARRAY_CELL_DROP;break;
    case XR_LIR_SET_BORROW:operation=XR_RBC_ARRAY_BORROW;break;
    case XR_LIR_SET_REF_READ:operation=XR_RBC_ARRAY_REF_READ;break;
    case XR_LIR_SET_REF_WRITE:operation=XR_RBC_ARRAY_REF_WRITE;break;
    case XR_LIR_CELL_NEW:operation=XR_RBC_ARRAY_CELL_NEW;break;
    case XR_LIR_CELL_READ:operation=XR_RBC_ARRAY_CELL_READ;break;
    case XR_LIR_CELL_WRITE:operation=XR_RBC_ARRAY_CELL_WRITE;break;
    case XR_LIR_STATE_READ:operation=XR_RBC_SET_STATE_READ;break;
    case XR_LIR_STATE_WRITE:operation=XR_RBC_SET_STATE_WRITE;break;
    default:return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"Set operation has no bytecode role");
    }
    XrLirType receiver=XR_LIR_TYPE_INVALID;
    if(i->operand_count)xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,i->operands[0]},&receiver);
    if(receiver==XR_LIR_SET_I64_REF) {
        if(operation==XR_RBC_SET_ADD)operation=XR_RBC_SET_REF_ADD;
        if(operation==XR_RBC_SET_REMOVE)operation=XR_RBC_SET_REF_REMOVE;
        if(operation==XR_RBC_SET_CLEAR)operation=XR_RBC_SET_REF_CLEAR;
    }
    XrRbcArrayAccess row={.operation=operation,.kind=XR_RBC_SET_I64,.count=i->operand_count};
    if(i->result)row.result=rbc_lir_array_location(c,i->result);
    if(operation==XR_RBC_ARRAY_CELL_NEW) {
        if(c->cell_count==65536)return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"Set cells exceed activation capacity");
        row.cell=c->cell_count;c->cell_kinds[c->cell_count++]=XR_RBC_SET_I64;
    }
    XrRbcArrayLocation operands[2];
    if(row.count>2)return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"Set role has excess operands");
    for(uint32_t p=0;p<row.count;++p)operands[p]=rbc_lir_array_location(c,i->operands[p]);
    return rbc_lir_array_append(c,row,operands);
}
