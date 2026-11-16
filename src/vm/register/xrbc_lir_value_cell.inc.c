/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_value_cell.inc.c - Exact Cell hooks from common lexical events
 */
static bool rbc_lir_cell_access(RbcLirCompiler *c,const XrLirInstructionView *i,bool *handled) {
    uint32_t kind=0,value=0;
    if (rbc_lir_captured_cell_point(c->input,c->function,i)) {
        kind=XR_LIR_OWNERSHIP_CELL_OPERATION; value=i->op==XR_LIR_CELL_NEW ? i->result : i->operands[0];
    } else if (i->op==XR_LIR_MAP_BORROW && rbc_lir_captured_cell(c->input,rbc_lir_value_type(c,i->operands[0]))) {
        kind=XR_LIR_OWNERSHIP_CELL_OPERATION; value=i->operands[0];
    } else if ((i->op==XR_LIR_VALUE_RETAIN || i->op==XR_LIR_FUNCTION_BIND) && rbc_lir_captured_cell(c->input,i->type)) {
        kind=XR_LIR_OWNERSHIP_CELL_VALUE; value=i->result;
    } else if (i->op==XR_LIR_VALUE_DROP && rbc_lir_captured_cell(c->input,rbc_lir_value_type(c,i->operands[0]))) {
        kind=XR_LIR_OWNERSHIP_RELEASE; value=i->operands[0];
    } else if (i->op==XR_LIR_FUNCTION_SCOPE_LEAVE && rbc_lir_captured_cell(c->input,i->type)) {
        kind=XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT; value=i->operands[1];
    } else if (i->op==XR_LIR_FUNCTION_SCOPE_LEAVE && !i->result &&
        !rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT,0) &&
        rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT,0)) {
        kind=XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT;
    }
    if (!kind) return true;
    XrRbcValueAccess access={.operation=XR_RBC_VALUE_CELL,.result=i->result,
        .source=i->operand_count ? i->operands[0] : 0,
        .other=i->operand_count>1 ? i->operands[1] : 0,
        .index=rbc_lir_callable_event(c,kind,value)};
    if (!access.index) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Cell hook lost its exact common event");
    if (i->op==XR_LIR_FUNCTION_BIND || i->op==XR_LIR_FUNCTION_SCOPE_LEAVE) {
        access.source=i->operand_count>1 ? i->operands[1] : 0; access.other=c->scope_home[i->operands[0]];
    }
    *handled=true;
    return rbc_lir_value_append(c,access) &&
        (i->op!=XR_LIR_FUNCTION_SCOPE_LEAVE || rbc_lir_function_releases(c));
}
