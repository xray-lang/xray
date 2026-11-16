/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_cell_ownership.inc.c - Events for verified lexical Cell identities
 */
static bool lir_plan_cell_row(const XrLirCallableFunctionFacts *facts, uint32_t value) {
    return value && value <= facts->value_count && facts->values[value].kind == XR_LIR_SCOPED_VALUE_CELL;
}
static XrLirCellOperation lir_plan_cell_operation(XrLirOp op) {
    switch (op) {
    case XR_LIR_CELL_NEW: return XR_LIR_CELL_OP_NEW;
    case XR_LIR_CELL_READ: return XR_LIR_CELL_OP_READ;
    case XR_LIR_CELL_WRITE: return XR_LIR_CELL_OP_WRITE;
    case XR_LIR_MAP_BORROW: return XR_LIR_CELL_OP_BORROW_MAP;
    case XR_LIR_VALUE_RETAIN: return XR_LIR_CELL_OP_COPY;
    case XR_LIR_FUNCTION_BIND: return XR_LIR_CELL_OP_BIND;
    case XR_LIR_FUNCTION_SCOPE_LEAVE: return XR_LIR_CELL_OP_KEEP;
    default: return XR_LIR_CELL_OP_NONE;
    }
}
static XrLirOwnershipFact lir_plan_cell_fact(XrLirCallableValueFact row, uint32_t function, uint32_t value) {
    return (XrLirOwnershipFact){.function=function,.block=row.block,.instruction=row.instruction,
        .value=value,.type=row.type,.cell_payload=row.cell_payload,.cell_transport=row.cell_transport,
        .cell_scope=row.owner_scope,.cell_operation=lir_plan_cell_operation(row.operation)};
}
static bool lir_plan_cell_exit(XrLirOwnershipPlan *plan,
    XrLirCallableFunctionFacts facts, const uint32_t *heads, const uint32_t *next,
    XrLirOwnershipFact position) {
    uint32_t kept = position.value;
    if (kept && lir_plan_cell_row(&facts,kept)) {
        position.type = facts.values[kept].type;
        position.cell_payload = facts.values[kept].cell_payload;
        position.cell_transport = facts.values[kept].cell_transport;
        position.transfer_mode = position.cell_transport == XR_LIR_CELL_TRANSPORT_BORROW ?
            XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_MOVE;
    } else position.value = position.target_value = 0;
    position.kind = XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT;
    if (!lir_plan_add(plan,position)) return false;
    for (uint32_t value = heads[position.cell_scope]; value; value = next[value]) {
        XrLirOwnershipFact release = lir_plan_cell_fact(facts.values[value],position.function,value);
        release.kind = XR_LIR_OWNERSHIP_RELEASE; release.block = position.block;
        release.instruction = position.instruction; release.cell_operation = XR_LIR_CELL_OP_NONE;
        if (!lir_plan_add(plan,release)) return false;
    }
    return true;
}
static XrLirStatus lir_plan_cell_events(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    uint32_t function, XrLirCallableFunctionFacts facts, const uint32_t *heads, const uint32_t *next) {
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t value = i + 1;
        if (!lir_plan_cell_row(&facts,value)) continue;
        XrLirCallableValueFact row = facts.values[value];
        XrLirOwnershipFact fact = lir_plan_cell_fact(row,function,value);
        fact.kind = XR_LIR_OWNERSHIP_CELL_VALUE; fact.target_value = row.alias_source;
        fact.argument = row.parameter ? row.parameter - 1 : row.instruction ? 0 : f->values[i].instruction;
        if (row.alias_source) fact.transfer_mode = row.operation == XR_LIR_FUNCTION_SCOPE_LEAVE &&
            facts.values[row.alias_source].cell_transport == XR_LIR_CELL_TRANSPORT_OWNED ?
            XR_LIR_OWNERSHIP_TRANSFER_MOVE : XR_LIR_OWNERSHIP_TRANSFER_COPY;
        if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
        fact.kind = row.parameter ? XR_LIR_OWNERSHIP_PARAMETER_BORROW : row.instruction ?
            XR_LIR_OWNERSHIP_RESULT_OWNER : XR_LIR_OWNERSHIP_BLOCK_OWNER;
        fact.transfer_mode = row.parameter ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE;
        if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            const XrLirInstructionView *v = &block->instructions[n].view;
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                uint32_t value = v->operands[p];
                if (!lir_plan_cell_row(&facts,value)) continue;
                XrLirOwnershipFact fact = lir_plan_cell_fact(facts.values[value],function,value);
                fact.block = b + 1; fact.instruction = n + 1; fact.argument = p;
                fact.cell_operation = XR_LIR_CELL_OP_NONE;
                fact.kind = v->op == XR_LIR_VALUE_DROP ? XR_LIR_OWNERSHIP_RELEASE :
                    v->op == XR_LIR_RET ? XR_LIR_OWNERSHIP_RETURN_TRANSFER : XR_LIR_OWNERSHIP_OPERAND_BORROW;
                fact.transfer_mode = v->op != XR_LIR_RET ? XR_LIR_OWNERSHIP_TRANSFER_BORROW :
                    fact.cell_transport == XR_LIR_CELL_TRANSPORT_BORROW ? XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_MOVE;
                if (v->op == XR_LIR_VALUE_DROP) fact.transfer_mode = XR_LIR_OWNERSHIP_TRANSFER_NONE;
                if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
            }
            uint32_t cell = v->op == XR_LIR_CELL_NEW ? v->result : v->operand_count ? v->operands[0] : 0;
            if (lir_plan_cell_row(&facts,cell) && (v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_CELL_READ ||
                v->op == XR_LIR_CELL_WRITE || v->op == XR_LIR_MAP_BORROW)) {
                XrLirOwnershipFact fact = lir_plan_cell_fact(facts.values[cell],function,cell);
                fact.kind = XR_LIR_OWNERSHIP_CELL_OPERATION; fact.block = b + 1; fact.instruction = n + 1;
                fact.cell_operation = lir_plan_cell_operation(v->op);
                fact.target_value = v->op == XR_LIR_CELL_NEW ? v->operands[0] :
                    v->op == XR_LIR_CELL_WRITE ? v->operands[1] : v->result;
                if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
            }
            if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE || v->op == XR_LIR_RET) {
                XrLirOwnershipFact exit = {.function=function,.block=b + 1,.instruction=n + 1,
                    .cell_scope=v->op == XR_LIR_RET ? 0 : v->operands[0],
                    .cell_operation=v->op == XR_LIR_RET ? XR_LIR_CELL_OP_NONE : XR_LIR_CELL_OP_KEEP,
                    .value=v->op == XR_LIR_RET ? 0 :
                        v->operand_count == 2 ? v->operands[1] : 0,.target_value=v->result};
                if (!lir_plan_cell_exit(plan,facts,heads,next,exit)) return XR_LIR_OUT_OF_MEMORY;
            }
            for (uint32_t e = 0; e < v->edge_count; ++e)
                for (uint32_t p = 0; p < v->edges[e].argument_count; ++p) {
                    uint32_t value = v->edges[e].arguments[p];
                    if (!lir_plan_cell_row(&facts,value)) continue;
                    XrLirOwnershipFact fact = lir_plan_cell_fact(facts.values[value],function,value);
                    fact.kind = XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER; fact.block = b + 1;
                    fact.instruction = n + 1; fact.edge = e + 1; fact.argument = p;
                    fact.target_block = v->edges[e].target;
                    fact.target_value = f->blocks[fact.target_block - 1].parameters[p];
                    fact.transfer_mode = XR_LIR_OWNERSHIP_TRANSFER_COPY;
                    if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
                }
        }
    }
    return XR_LIR_OK;
}
static XrLirStatus lir_plan_cells(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    uint32_t function, const XrLirCallableFacts *cached) {
    XrLirCallableFunctionFacts facts = {0}; bool needed = false;
    if (!cached || xr_lir_callable_facts_function(cached,function,&facts) != XR_LIR_OK || !facts.values) return XR_LIR_OK;
    for (uint32_t v = 0; v < facts.value_count; ++v) needed |= lir_plan_cell_row(&facts,v + 1);
    if (!needed) return XR_LIR_OK;
    size_t count = (size_t)f->value_count + 1;
    if (!count || count > SIZE_MAX / (2 * sizeof(uint32_t))) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *heads = xr_calloc(count,2 * sizeof(*heads));
    if (!heads) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *next = heads + count;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        uint32_t value = v + 1;
        if (!lir_plan_cell_row(&facts,value) || facts.values[value].cell_transport != XR_LIR_CELL_TRANSPORT_OWNED) continue;
        uint32_t scope = facts.values[value].owner_scope;
        if (scope > f->value_count) { xr_free(heads); return XR_LIR_VERIFY_ERROR; }
        next[value] = heads[scope]; heads[scope] = value;
    }
    XrLirStatus status = lir_plan_cell_events(plan,f,function,facts,heads,next);
    xr_free(heads); return status;
}
