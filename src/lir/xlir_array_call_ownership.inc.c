/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Local lifetime facts for moving an owning Array to a direct callee.
 */

typedef struct LirArrayCallUse {
    LirBindingForwardUse position;
    uint32_t last_observation, release;
    bool observed_more_than_once;
} LirArrayCallUse;

static bool lir_array_call_i64(const XrLirValueData *value) {
    return value->type == XR_LIR_ARRAY && value->array_element == XR_LIR_I64;
}

static bool lir_array_call_operation(const XrLirOwnershipFact *fact) {
    return fact->type == XR_LIR_ARRAY && fact->array_element == XR_LIR_I64 &&
        (fact->kind == XR_LIR_OWNERSHIP_OPERAND_BORROW || fact->kind == XR_LIR_OWNERSHIP_RELEASE ||
            fact->kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER || fact->kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER);
}

/* Derive this from the function's own code only. Direct call binding below
 * consults the result, so an eventual module interface needs only the formal
 * ordinal and its take capability, not the callee body or linked LIR IDs. */
static XrLirArrayEntryRetain lir_array_entry_retain(const XrLirFunctionData *f,
    const XrLirOwnershipFact *facts, uint32_t count) {
    XrLirArrayEntryRetain result = {0};
    if (f->kind != XR_LIR_FUNCTION_DEFINITION || f->closure.callable || f->result != XR_LIR_ARRAY ||
        f->array_result != XR_LIR_I64) return result;
    uint32_t formal = 0, parameter = 0;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        uint32_t value = f->parameters[p];
        if (lir_array_call_i64(&f->values[value - 1])) {
            if (formal) return result;
            formal = value;
            parameter = p;
        } else if (!lir_array_region_scalar(f->parameter_types[p])) return result;
    }
    if (!formal) return result;
    uint32_t retain = 0;
    for (uint32_t n = 0; n < count; ++n) {
        const XrLirOwnershipFact *fact = &facts[n];
        if (fact->value != formal || !lir_array_call_operation(fact)) continue;
        if (retain || fact->kind != XR_LIR_OWNERSHIP_OPERAND_BORROW || fact->block != f->entry) return result;
        retain = fact->instruction;
    }
    if (!retain) return result;
    const XrLirBlockData *entry = &f->blocks[f->entry - 1];
    const XrLirInstructionView *op = &entry->instructions[retain - 1].view;
    if (op->op != XR_LIR_ARRAY_RETAIN) return result;
    for (uint32_t n = 0; n + 1 < retain; ++n)
        if (!lir_array_region_pure(f, &entry->instructions[n].view)) return result;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) if (term->edges[e].target == f->entry) return result;
    }
    return (XrLirArrayEntryRetain){parameter, f->entry, retain, op->result};
}

static void lir_array_call_uses(LirArrayCallUse *uses, const XrLirOwnershipFact *facts, uint32_t count) {
    for (uint32_t n = 0; n < count; ++n) {
        const XrLirOwnershipFact *fact = &facts[n];
        if (!lir_array_call_operation(fact)) continue;
        LirArrayCallUse *use = &uses[fact->value];
        lir_forward_use(&use->position, fact->block, fact->instruction);
        if (fact->kind == XR_LIR_OWNERSHIP_RELEASE) {
            if (use->release) use->position.blocked = true;
            use->release = fact->instruction;
        } else {
            use->observed_more_than_once |= use->last_observation != 0;
            if (use->last_observation < fact->instruction) use->last_observation = fact->instruction;
        }
    }
}

static bool lir_array_call_move(const XrLirFunctionData *f, const XrLirInstructionView *call,
    const LirArrayCallUse *uses, XrLirArrayCallMove *move, uint32_t parameter, uint32_t barrier) {
    uint32_t temporary = call->operands[parameter];
    const XrLirValueData *value = &f->values[temporary - 1];
    if (value->parameter || value->block != move->block || value->instruction + 1 != barrier) return false;
    const XrLirInstructionView *retain = xr_lir_internal_value_definition(f, temporary);
    if (!retain || retain->op != XR_LIR_ARRAY_RETAIN) return false;
    uint32_t owner = retain->operands[0];
    if (f->values[owner - 1].block != move->block) return false; /* Not a borrowed formal. */
    const LirArrayCallUse *source = &uses[owner], *stage = &uses[temporary];
    if (source->position.blocked || source->position.block != move->block || source->last_observation != barrier ||
        source->release <= move->instruction || stage->position.blocked || stage->position.block != move->block ||
        stage->observed_more_than_once || stage->last_observation != move->instruction || stage->release <= move->instruction)
        return false;
    const XrLirBlockData *block = &f->blocks[move->block - 1];
    if (block->instructions[source->release - 1].view.op != XR_LIR_ARRAY_DROP ||
        block->instructions[stage->release - 1].view.op != XR_LIR_ARRAY_DROP) return false;
    move->owner = owner; move->temporary = temporary; move->retain = barrier;
    move->owner_drop = source->release; move->temporary_drop = stage->release;
    return true;
}

static XrLirStatus lir_array_calls_in_function(const XrLirFunctionData *f, XrLirOwnershipPlan *plan,
    XrLirOwnershipFunction *row) {
    row->array_call_offset = plan->array_calls.count;
    LirArrayCallUse *uses = NULL;
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t b = 0; b < f->block_count && status == XR_LIR_OK; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        uint32_t barrier = 0;
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *op = &block->instructions[i].view;
            if (op->op == XR_LIR_CALL) {
                XrLirArrayEntryRetain callee = plan->functions[op->callee - 1].array_entry_retain;
                if (callee.instruction) {
                    if (!uses) {
                        uint64_t count = (uint64_t)f->value_count + 1;
                        if (count <= SIZE_MAX / sizeof(*uses)) uses = lir_plan_allocate((size_t)count, sizeof(*uses));
                        if (!uses) { status = XR_LIR_OUT_OF_MEMORY; break; }
                        lir_array_call_uses(uses, plan->facts + row->fact_offset, row->fact_count);
                    }
                    XrLirArrayCallMove move = {.block=b + 1, .instruction=i + 1, .callee=op->callee};
                    if (lir_array_call_move(f, op, uses, &move, callee.parameter, barrier) &&
                        !lir_plan_array_append(&plan->array_calls, &move, sizeof(move))) {
                        status = XR_LIR_OUT_OF_MEMORY;
                        break;
                    }
                }
            }
            /* Keep the old carrier when later argument evaluation might act
             * or fail between the staged retain and the direct call. */
            if (!lir_array_region_pure(f, op)) barrier = i + 1;
        }
    }
    xr_free(uses);
    row->array_call_count = plan->array_calls.count - row->array_call_offset;
    return status;
}

static XrLirStatus lir_plan_array_calls(const XrLirModule *m, XrLirOwnershipPlan *plan) {
    bool has_receiver = false;
    for (uint32_t f = 0; f < plan->function_count; ++f) {
        XrLirOwnershipFunction *row = &plan->functions[f];
        if (!row->fact_count) continue;
        row->array_entry_retain = lir_array_entry_retain(&m->functions[f], plan->facts + row->fact_offset, row->fact_count);
        has_receiver |= row->array_entry_retain.instruction != 0;
    }
    if (!has_receiver) return XR_LIR_OK;
    for (uint32_t f = 0; f < plan->function_count; ++f) {
        XrLirOwnershipFunction *row = &plan->functions[f];
        if (!row->fact_count) continue;
        XrLirStatus status = lir_array_calls_in_function(&m->functions[f], plan, row);
        if (status != XR_LIR_OK) return status;
    }
    return XR_LIR_OK;
}
