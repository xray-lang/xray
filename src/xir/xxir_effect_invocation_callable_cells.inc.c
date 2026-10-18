/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_callable_cells.inc.c - Complete local callable Cell sources
 *
 * A direct allocation with no identity escape has only its physical initializer
 * and writes as payload sources. All of them join the existing invocation row;
 * the original opaque advertisement remains. Aliases and escapes stay unknown.
 */
static XrXirStatus effect_invocation_callable_cell(EffectInvocationFlow *flow,
    const XrXirInstruction *read,uint32_t *output) {
    if(!output || !read || read->op!=XR_XIR_CELL_READ) return XR_XIR_BAD_STRUCTURE;
    *output=UINT32_MAX;
    if(!xr_xir_callable_signature(flow->body.domain.types,read->type)) return XR_XIR_OK;
    uint32_t cell=read->args[0];
    if(cell<flow->function->parameter_count || cell>=flow->values) return XR_XIR_OK;
    XrXirInstruction storage;const XrXirInstruction *allocation=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,
        cell-flow->function->parameter_count,&storage,&allocation);
    if(status!=XR_XIR_OK || allocation->op!=XR_XIR_CELL_NEW) return status;
    EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if(status!=XR_XIR_OK) return status;
    bool escaped=false;
    for(uint32_t i=0;i<flow->function->instruction_count;++i) {
        if(!xir_compile_work(flow->read->work,4)) return XR_XIR_BUDGET;
        XrXirInstruction decoded;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_operation(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&decoded,&op);
        if(status!=XR_XIR_OK) return status;
        if(op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        bool table=xr_xir_op_uses_operand_table(op->op);
        uint32_t count=table?op->args[1]:effect_parameter_operands[op->op];
        if(table && (op->args[0]>flow->function->operand_count ||
            count>flow->function->operand_count-op->args[0] || (count && !flow->function->operands)))
            return XR_XIR_BAD_STRUCTURE;
        if(!table && count>XR_COUNTOF(op->args)) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t a=0;a<count;++a) {
            if(!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
            if(op->op==XR_XIR_PHI && !(a&1)) continue;
            uint32_t value=table?flow->function->operands[op->args[0]+a]:op->args[a];
            if(value!=cell) continue;
            if(a || (op->op!=XR_XIR_CELL_READ && op->op!=XR_XIR_CELL_WRITE &&
                op->op!=XR_XIR_CELL_LOCAL_WRITE)) escaped=true;
        }
    }
    status=effect_body_request_sequence_finish(&flow->body,
        flow->read->final->nodes[flow->node].body,&cursor);
    if(status==XR_XIR_OK && !escaped) *output=cell;
    return status;
}

static XrXirStatus effect_invocation_callable_cell_source(EffectInvocationFlow *flow,
    uint32_t cell,uint32_t instruction,const XrXirInstruction *op,uint32_t *output) {
    if(!output || !op || cell>=flow->values || instruction>=flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    *output=UINT32_MAX;
    if(op->op==XR_XIR_CELL_NEW && flow->function->parameter_count+instruction==cell)
        *output=op->args[0];
    else if((op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) && op->args[0]==cell)
        *output=op->args[1];
    if(*output!=UINT32_MAX && *output>=flow->values) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_callable_cell_origins(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *read) {
    uint32_t cell=UINT32_MAX;
    XrXirStatus status=effect_invocation_callable_cell(flow,read,&cell);
    if(status!=XR_XIR_OK || cell==UINT32_MAX) return status;
    EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<flow->function->instruction_count;++i) {
        if(!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
        XrXirInstruction decoded;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_operation(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&decoded,&op);
        uint32_t source=UINT32_MAX;
        if(status==XR_XIR_OK) status=effect_invocation_callable_cell_source(flow,cell,i,op,&source);
        if(status==XR_XIR_OK && source!=UINT32_MAX)
            status=effect_invocation_transfer(flow,flow->function->parameter_count+instruction,source);
        if(status!=XR_XIR_OK) return status;
    }
    return effect_body_request_sequence_finish(&flow->body,
        flow->read->final->nodes[flow->node].body,&cursor);
}
