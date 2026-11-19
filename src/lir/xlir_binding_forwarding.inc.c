/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_binding_forwarding.inc.c - Adjacent local reference handoff preparation
 */
typedef struct LirBindingForwardUse {
    uint32_t block, last;
    bool local, blocked;
} LirBindingForwardUse;

static bool lir_forward_cell(const XrLirFunctionData *f, uint32_t value, XrLirType type) {
    const XrLirValueData *row=&f->values[value-1];
    if (row->type!=type || row->parameter || !row->block ||
        (type!=XR_LIR_MAP_STRING_I64_CELL && (type!=XR_LIR_ARRAY_CELL || row->array_element!=XR_LIR_I64)))
        return false;
    const XrLirInstructionView *definition=xr_lir_internal_value_definition(f,value);
    return definition && definition->op==XR_LIR_CELL_NEW;
}
static bool lir_forward_role(const XrLirModule *m, const XrLirInstructionView *op,
    uint32_t argument, XrLirType type) {
    if (argument) return false;
    if (type==XR_LIR_MAP_STRING_I64_CELL) {
        if (op->op==XR_LIR_CELL_READ) return op->type==XR_LIR_MAP_STRING_I64;
        if (op->op==XR_LIR_MAP_CELL_DROP) return true;
        XrLirIntrinsic entry={0};
        return op->op==XR_LIR_CALL_INTRINSIC && xr_lir_intrinsic_view(m,op->intrinsic,&entry)==XR_LIR_OK &&
            xr_lir_map_intrinsic_valid(m,&entry) && (entry.operation==XR_LIR_MAP_OP_SET ||
            entry.operation==XR_LIR_MAP_OP_REMOVE || entry.operation==XR_LIR_MAP_OP_CLEAR);
    }
    if (op->op==XR_LIR_CELL_READ) return op->type==XR_LIR_ARRAY;
    if (op->op==XR_LIR_ARRAY_CELL_DROP) return true;
    const XrLirArrayIntrinsic *shape=op->op==XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_array_intrinsic(m,op->intrinsic) : NULL;
    return shape && shape->parameter_count && shape->parameter_elements && shape->parameter_elements[0]==XR_LIR_I64 &&
        (shape->operation==XR_LIR_ARRAY_OP_SET || shape->operation==XR_LIR_ARRAY_OP_PUSH);
}
static void lir_forward_use(LirBindingForwardUse *use, uint32_t block, uint32_t instruction) {
    if (!use->block) use->block=block;
    else if (use->block!=block) use->blocked=true;
    if (instruction>use->last) use->last=instruction;
}
/* Verified logical ownership already checks releases and reachability. One
 * linear operand inventory only limits physical candidates to unaliased local
 * Cell roles; it is not another liveness or semantic-effect solver. */
static XrLirStatus lir_plan_binding_forwarding(XrLirOwnershipPlan *plan,
    const XrLirModule *m, const XrLirFunctionData *f, uint32_t function) {
    bool needed=false;
    for (uint32_t v=0;v<f->value_count && !needed;++v) needed=lir_forward_cell(f,v+1,f->values[v].type);
    if (!needed) return XR_LIR_OK;
    uint64_t count=(uint64_t)f->value_count+1;
    if (count>SIZE_MAX/sizeof(LirBindingForwardUse)) return XR_LIR_OUT_OF_MEMORY;
    LirBindingForwardUse *uses=lir_plan_allocate((size_t)count,sizeof(*uses));
    if (!uses) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t v=0;v<f->value_count;++v) uses[v+1].local=lir_forward_cell(f,v+1,f->values[v].type);
    for (uint32_t b=0;b<f->block_count;++b) {
        const XrLirBlockData *block=&f->blocks[b];
        for (uint32_t i=0;i<block->instruction_count;++i) {
            const XrLirInstructionView *op=&block->instructions[i].view;
            for (uint32_t a=0;a<op->operand_count;++a) {
                uint32_t value=op->operands[a];
                LirBindingForwardUse *use=&uses[value];
                lir_forward_use(use,b+1,i+1);
                if (use->local && !lir_forward_role(m,op,a,f->values[value-1].type)) use->blocked=true;
            }
            for (uint32_t e=0;e<op->edge_count;++e)
                for (uint32_t a=0;a<op->edges[e].argument_count;++a)
                    lir_forward_use(&uses[op->edges[e].arguments[a]],b+1,i+1);
        }
    }
    XrLirOwnershipFunction row=plan->functions[function-1];
    for (uint32_t n=0;n<row.fact_count;++n) {
        XrLirOwnershipFact *fact=&plan->facts[row.fact_offset+n];
        if ((fact->kind!=XR_LIR_OWNERSHIP_BINDING_NEW && fact->kind!=XR_LIR_OWNERSHIP_BINDING_READ) ||
            !fact->value || !fact->target_value || !fact->block || !fact->instruction) continue;
        const LirBindingForwardUse *binding=&uses[fact->value];
        if (!binding->local || binding->blocked || binding->block!=fact->block) continue;
        bool map=fact->type==XR_LIR_MAP_STRING_I64_CELL;
        if (!map && (fact->type!=XR_LIR_ARRAY_CELL || fact->array_element!=XR_LIR_I64)) continue;
        const XrLirValueData *cell=&f->values[fact->value-1],*value=&f->values[fact->target_value-1];
        const XrLirBlockData *block=&f->blocks[fact->block-1];
        if (cell->block!=fact->block || fact->instruction>=block->instruction_count ||
            (map ? value->type!=XR_LIR_MAP_STRING_I64 : value->type!=XR_LIR_ARRAY || value->array_element!=XR_LIR_I64))
            continue;
        const XrLirInstructionView *op=&block->instructions[fact->instruction-1].view;
        const XrLirInstructionView *drop=&block->instructions[fact->instruction].view;
        bool matched=false;
        if (fact->kind==XR_LIR_OWNERSHIP_BINDING_NEW) {
            const LirBindingForwardUse *source=&uses[fact->target_value];
            matched=op->op==XR_LIR_CELL_NEW && op->result==fact->value && op->operand_count==1 &&
                op->operands[0]==fact->target_value && value->block==fact->block &&
                (!value->parameter || value->block) && !source->blocked && source->block==fact->block &&
                source->last==fact->instruction+1 && drop->op==(map ? XR_LIR_MAP_DROP : XR_LIR_ARRAY_DROP) &&
                drop->operand_count==1 && drop->operands[0]==fact->target_value;
        } else {
            matched=op->op==XR_LIR_CELL_READ && op->result==fact->target_value && op->operand_count==1 &&
                op->operands[0]==fact->value && binding->last==fact->instruction+1 &&
                drop->op==(map ? XR_LIR_MAP_CELL_DROP : XR_LIR_ARRAY_CELL_DROP) &&
                drop->operand_count==1 && drop->operands[0]==fact->value;
        }
        if (matched) {
            fact->forwarding_mode=XR_LIR_OWNERSHIP_TRANSFER_MOVE;
            fact->forwarding_drop=fact->instruction+1;
        }
    }
    xr_free(uses); return XR_LIR_OK;
}
