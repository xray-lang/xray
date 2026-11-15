/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_ownership.inc.c - Cold collection within the existing verifier
 */
struct XrLirOwnershipPlan {
    XrLirOwnershipFunction *functions;
    XrLirOwnershipFact *facts;
    uint32_t function_count, fact_count, fact_capacity;
    XrLirOwnershipTypeFact *types;
    uint32_t type_count;
};

static XrLirStatus lir_verify(const XrLirModule *, XrLirDiagnostic *, XrLirCallableFacts **);

static void *lir_plan_allocate(size_t count, size_t size) {
    return size && count && count <= SIZE_MAX / size ? xr_calloc(count, size) : NULL;
}

static const XrLirOwnershipTypeFact *lir_plan_type(const XrLirOwnershipPlan *plan, XrLirType type) {
    uint32_t first = 0, count = plan->type_count;
    while (count) {
        uint32_t middle = first + count / 2;
        if (plan->types[middle].type < type) {
            count -= middle - first + 1;
            first = middle + 1;
        } else count = middle - first;
    }
    return first < plan->type_count && plan->types[first].type == type ? &plan->types[first] : NULL;
}
static const XrLirTypeData *lir_plan_constructed_type(const XrLirModule *m, XrLirType type) {
    if (type < XR_LIR_TYPE_COUNT || !xr_lir_internal_type_valid(m, type, true)) return NULL;
    return &m->types[type - XR_LIR_TYPE_COUNT];
}
static bool lir_plan_constructed_value(const XrLirModule *m, XrLirType type) {
    const XrLirTypeData *value = lir_plan_constructed_type(m, type);
    return value && (value->shape.kind == XR_LIR_TYPE_RECORD || value->shape.kind == XR_LIR_TYPE_NULLABLE);
}
static uint32_t lir_plan_unmodeled(const XrLirModule *m, const XrLirOwnershipPlan *plan, XrLirType type) {
    switch (type) {
        case XR_LIR_I64_CELL: case XR_LIR_STRING_CELL: case XR_LIR_ARRAY_CELL:
        case XR_LIR_MAP_STRING_I64_CELL: return XR_LIR_OWNERSHIP_UNMODELED_CELL;
        case XR_LIR_STATE_REF: case XR_LIR_ARRAY_REF: case XR_LIR_STRING_REF:
        case XR_LIR_MAP_STRING_I64_REF: return XR_LIR_OWNERSHIP_UNMODELED_REFERENCE;
        case XR_LIR_STATE: return XR_LIR_OWNERSHIP_UNMODELED_STATE;
        case XR_LIR_VIEW_FRAME: case XR_LIR_VIEW_CELL: case XR_LIR_SLICE_I64:
            return XR_LIR_OWNERSHIP_UNMODELED_VIEW;
        case XR_LIR_FUNCTION_SCOPE: return XR_LIR_OWNERSHIP_UNMODELED_CALLABLE;
        default:
            if (type < XR_LIR_TYPE_COUNT || lir_plan_type(plan, type)) return 0;
            const XrLirTypeData *value = lir_plan_constructed_type(m, type);
            if (!value) return XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED;
            if (value->shape.kind == XR_LIR_TYPE_CALLABLE)
                return XR_LIR_OWNERSHIP_UNMODELED_CALLABLE;
            if (value->shape.kind == XR_LIR_TYPE_CELL)
                return XR_LIR_OWNERSHIP_UNMODELED_CELL;
            return XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED;
    }
}

static bool lir_plan_function(XrLirOwnershipPlan *plan, const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function) {
    if (!plan) return true;
    XrLirOwnershipFunction *out = &plan->functions[function - 1];
    out->fact_offset = plan->fact_count;
    out->external = f->kind == XR_LIR_FUNCTION_EXTERNAL;
    if (lir_plan_constructed_value(m, f->result) && !lir_plan_type(plan, f->result)) return false;
    out->unmodeled = lir_plan_unmodeled(m, plan, f->result);
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        XrLirType type = f->parameter_types[p];
        if (lir_plan_constructed_value(m, type) && !lir_plan_type(plan, type)) return false;
        out->unmodeled |= lir_plan_unmodeled(m, plan, type);
    }
    for (uint32_t v = 0; v < f->value_count; ++v) {
        XrLirType type = f->values[v].type;
        if (lir_plan_constructed_value(m, type) && !lir_plan_type(plan, type)) return false;
        out->unmodeled |= lir_plan_unmodeled(m, plan, type);
    }
    return true;
}

static bool lir_plan_add(XrLirOwnershipPlan *plan, XrLirOwnershipFact fact) {
    if (!plan) return true;
    const XrLirOwnershipTypeFact *type = lir_plan_type(plan, fact.type);
    if (type && !type->has_owned) return true;
    if (plan->fact_count == UINT32_MAX) return false;
    if (plan->fact_count == plan->fact_capacity) {
        uint32_t capacity = plan->fact_capacity > UINT32_MAX / 2 ? UINT32_MAX :
            plan->fact_capacity ? plan->fact_capacity * 2 : 32;
        XrLirOwnershipFact *facts = lir_plan_allocate(capacity, sizeof(*facts));
        if (!facts) return false;
        if (plan->fact_count)
            memcpy(facts, plan->facts, (size_t)plan->fact_count * sizeof(*facts));
        xr_free(plan->facts);
        plan->facts = facts;
        plan->fact_capacity = capacity;
    }
    plan->facts[plan->fact_count++] = fact;
    ++plan->functions[fact.function - 1].fact_count;
    return true;
}

static bool lir_plan_value(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    XrLirOwnershipFactKind kind, uint32_t function, uint32_t block,
    uint32_t instruction, uint32_t value, uint32_t argument) {
    if (!plan) return true;
    XrLirOwnershipTransferMode mode = XR_LIR_OWNERSHIP_TRANSFER_NONE;
    if (kind == XR_LIR_OWNERSHIP_PARAMETER_BORROW || kind == XR_LIR_OWNERSHIP_OPERAND_BORROW)
        mode = XR_LIR_OWNERSHIP_TRANSFER_BORROW;
    else if (kind == XR_LIR_OWNERSHIP_APPEND_TRANSFER || kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER)
        mode = XR_LIR_OWNERSHIP_TRANSFER_MOVE;
    return lir_plan_add(plan, (XrLirOwnershipFact){.kind=kind, .function=function,
        .block=block, .instruction=instruction, .value=value, .argument=argument,
        .type=f->values[value - 1].type, .array_element=f->values[value - 1].array_element,.transfer_mode=mode});
}

/* Both lifetime checking and event collection consume this transfer identity. */
static XrLirOwnershipFactKind lir_plan_transfer(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    if (v->op == XR_LIR_RET && v->operand_count && lir_managed_value(m,f->result))
        return XR_LIR_OWNERSHIP_RETURN_TRANSFER;
    if (xr_lir_string_role(m,v) == XR_LIR_STRING_OP_APPEND_OWNED) return XR_LIR_OWNERSHIP_APPEND_TRANSFER;
    if (v->op == XR_LIR_STRING_DROP || v->op == XR_LIR_ARRAY_DROP ||
        v->op == XR_LIR_VALUE_DROP || v->op == XR_LIR_MAP_DROP || v->op == XR_LIR_SET_DROP) return XR_LIR_OWNERSHIP_RELEASE;
    return 0;
}
static bool lir_plan_binding(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    const XrLirInstructionView *v, uint32_t function, uint32_t block, uint32_t instruction) {
    XrLirOwnershipFact fact = {.function=function,.block=block,.instruction=instruction};
    switch (v->op) {
    case XR_LIR_CELL_NEW:
        if (!lir_cfg_cell(v->type)) return true;
        fact.kind=XR_LIR_OWNERSHIP_BINDING_NEW; fact.value=v->result; fact.target_value=v->operands[0]; break;
    case XR_LIR_ARRAY_CELL_DROP: case XR_LIR_MAP_CELL_DROP:
        fact.kind=XR_LIR_OWNERSHIP_BINDING_RELEASE; fact.value=v->operands[0]; break;
    case XR_LIR_ARRAY_BORROW: case XR_LIR_MAP_BORROW:
        fact.kind=XR_LIR_OWNERSHIP_BINDING_BORROW; fact.value=v->result; fact.target_value=v->operands[0]; break;
    case XR_LIR_CELL_READ:
        if (!lir_cfg_cell(f->values[v->operands[0] - 1].type)) return true;
        fact.kind=XR_LIR_OWNERSHIP_BINDING_READ; fact.value=v->operands[0]; fact.target_value=v->result; break;
    case XR_LIR_ARRAY_REF_READ: case XR_LIR_MAP_REF_READ:
        fact.kind=XR_LIR_OWNERSHIP_BINDING_READ; fact.value=v->operands[0]; fact.target_value=v->result; break;
    case XR_LIR_CELL_WRITE:
        if (!lir_cfg_cell(f->values[v->operands[0] - 1].type)) return true;
        fact.kind=XR_LIR_OWNERSHIP_BINDING_WRITE; fact.value=v->operands[0]; fact.target_value=v->operands[1]; break;
    case XR_LIR_ARRAY_REF_WRITE: case XR_LIR_MAP_REF_WRITE:
        fact.kind=XR_LIR_OWNERSHIP_BINDING_WRITE; fact.value=v->operands[0]; fact.target_value=v->operands[1]; break;
    default: return true;
    }
    fact.type=f->values[fact.value - 1].type;
    fact.array_element=f->values[fact.value - 1].array_element;
    return lir_plan_add(plan,fact);
}
static bool lir_plan_callable_row(const XrLirCallableFunctionFacts *facts, uint32_t value) {
    return value && value <= facts->value_count && facts->values[value].type &&
        facts->values[value].type != XR_LIR_FUNCTION_SCOPE;
}
static bool lir_plan_callable_owned(const XrLirCallableFunctionFacts *facts, uint32_t value) {
    return lir_plan_callable_row(facts,value) && facts->values[value].transport == XR_LIR_CALLABLE_TRANSPORT_OWNED;
}
static XrLirOwnershipTransferMode lir_plan_callable_mode(XrLirCallableTransport transport,
    XrLirOwnershipTransferMode owned, XrLirOwnershipTransferMode borrowed) {
    if (transport == XR_LIR_CALLABLE_TRANSPORT_OWNED) return owned;
    if (transport == XR_LIR_CALLABLE_TRANSPORT_FRAME || transport == XR_LIR_CALLABLE_TRANSPORT_BORROW) return borrowed;
    return XR_LIR_OWNERSHIP_TRANSFER_NONE;
}
static XrLirStatus lir_plan_callable_events(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    uint32_t function, XrLirCallableFunctionFacts facts, const uint32_t *scope_heads, const uint32_t *next) {
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t value = i + 1;
        if (!lir_plan_callable_row(&facts,value)) continue;
        XrLirCallableValueFact row = facts.values[value];
        XrLirOwnershipTransferMode mode = XR_LIR_OWNERSHIP_TRANSFER_NONE;
        if (row.operation == XR_LIR_FUNCTION_BIND || row.operation == XR_LIR_FUNCTION_WEAKEN)
            mode = lir_plan_callable_mode(row.transport,XR_LIR_OWNERSHIP_TRANSFER_COPY,XR_LIR_OWNERSHIP_TRANSFER_BORROW);
        else if (row.operation == XR_LIR_FUNCTION_SCOPE_LEAVE)
            mode = lir_plan_callable_mode(row.transport,XR_LIR_OWNERSHIP_TRANSFER_MOVE,XR_LIR_OWNERSHIP_TRANSFER_BORROW);
        XrLirOwnershipFact fact = {.kind=XR_LIR_OWNERSHIP_CALLABLE_VALUE,.function=function,
            .block=row.block,.instruction=row.instruction,.value=value,
            .argument=row.parameter ? row.parameter - 1 : row.instruction ? 0 : f->values[value - 1].instruction,
            .target_value=row.alias_source,.type=row.type,.callable_transport=row.transport,.callable_operation=row.operation,
            .callable_function=row.operation == XR_LIR_FUNCTION_REF ? row.known_target : 0,
            .callable_scope=row.owner_scope,.callable_materialize=row.materialize,.transfer_mode=mode};
        if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
        if (row.parameter && !lir_plan_value(plan,f,XR_LIR_OWNERSHIP_PARAMETER_BORROW,function,0,0,value,row.parameter - 1))
            return XR_LIR_OUT_OF_MEMORY;
        if (row.transport == XR_LIR_CALLABLE_TRANSPORT_OWNED) {
            fact.kind = row.instruction ? XR_LIR_OWNERSHIP_RESULT_OWNER : XR_LIR_OWNERSHIP_BLOCK_OWNER;
            if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
        }
        if (row.materialize) {
            fact.kind = XR_LIR_OWNERSHIP_CALLABLE_MATERIALIZE;
            if (!lir_plan_add(plan,fact)) return XR_LIR_OUT_OF_MEMORY;
        }
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            const XrLirInstructionView *v = &block->instructions[n].view;
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                uint32_t value = v->operands[p];
                if (!lir_plan_callable_row(&facts,value)) continue;
                XrLirOwnershipFactKind kind = v->op == XR_LIR_RET ?
                    XR_LIR_OWNERSHIP_RETURN_TRANSFER : XR_LIR_OWNERSHIP_OPERAND_BORROW;
                XrLirCallableTransport transport = facts.values[value].transport;
                XrLirOwnershipTransferMode mode = kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER ?
                    lir_plan_callable_mode(transport,XR_LIR_OWNERSHIP_TRANSFER_MOVE,XR_LIR_OWNERSHIP_TRANSFER_COPY) :
                    XR_LIR_OWNERSHIP_TRANSFER_BORROW;
                if (!lir_plan_add(plan,(XrLirOwnershipFact){.kind=kind,.function=function,.block=b + 1,.instruction=n + 1,
                    .value=value,.argument=p,.type=f->values[value - 1].type,.callable_transport=transport,.transfer_mode=mode}))
                    return XR_LIR_OUT_OF_MEMORY;
            }
            if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) {
                uint32_t scope = v->operands[0];
                uint32_t keep = v->operand_count == 2 ? v->operands[1] : 0;
                XrLirOwnershipTransferMode mode = !keep ? XR_LIR_OWNERSHIP_TRANSFER_NONE :
                    lir_plan_callable_mode(facts.values[keep].transport,XR_LIR_OWNERSHIP_TRANSFER_MOVE,XR_LIR_OWNERSHIP_TRANSFER_BORROW);
                if (!lir_plan_add(plan,(XrLirOwnershipFact){.kind=XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT,
                    .function=function,.block=b + 1,.instruction=n + 1,
                    .value=keep,.target_value=v->result,
                    .callable_scope=scope,.callable_operation=v->op,.transfer_mode=mode})) return XR_LIR_OUT_OF_MEMORY;
                for (uint32_t value = scope_heads[scope]; value; value = next[value])
                    if (!lir_plan_value(plan,f,XR_LIR_OWNERSHIP_RELEASE,function,b + 1,n + 1,value,0)) return XR_LIR_OUT_OF_MEMORY;
            }
        }
        const XrLirInstructionView *end = &block->instructions[block->instruction_count - 1].view;
        for (uint32_t e = 0; e < end->edge_count; ++e) {
            const XrLirEdgeView *edge = &end->edges[e];
            for (uint32_t p = 0; p < edge->argument_count; ++p) {
                uint32_t value = edge->arguments[p];
                if (!lir_plan_callable_row(&facts,value)) continue;
                if (!lir_plan_add(plan,(XrLirOwnershipFact){.kind=XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER,
                    .function=function,.block=b + 1,.instruction=block->instruction_count,.value=value,.argument=p,
                    .edge=e + 1,.target_block=edge->target,.target_value=f->blocks[edge->target - 1].parameters[p],
                    .type=f->values[value - 1].type,.callable_transport=facts.values[value].transport,
                    .transfer_mode=lir_plan_callable_mode(facts.values[value].transport,
                        XR_LIR_OWNERSHIP_TRANSFER_COPY,XR_LIR_OWNERSHIP_TRANSFER_BORROW)})) return XR_LIR_OUT_OF_MEMORY;
            }
        }
    }
    return XR_LIR_OK;
}
static XrLirStatus lir_plan_callables(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    uint32_t function, const XrLirCallableFacts *callables) {
    XrLirCallableFunctionFacts facts = {0};
    if (!callables || xr_lir_callable_facts_function(callables,function,&facts) != XR_LIR_OK || !facts.values) return XR_LIR_OK;
    if (facts.value_count != f->value_count) return XR_LIR_VERIFY_ERROR;
    size_t count = (size_t)f->value_count + 1;
    if (!count || count > SIZE_MAX / (2 * sizeof(uint32_t))) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *heads = xr_calloc(count,2 * sizeof(*heads));
    if (!heads) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *next = heads + count;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t value = i + 1;
        if (!lir_plan_callable_owned(&facts,value)) continue;
        uint32_t scope = facts.values[value].owner_scope;
        if (scope > f->value_count) { xr_free(heads); return XR_LIR_VERIFY_ERROR; }
        next[value] = heads[scope]; heads[scope] = value;
    }
    XrLirStatus status = lir_plan_callable_events(plan,f,function,facts,heads,next);
    xr_free(heads);
    return status;
}
/* The verifier establishes ownership once. This one deterministic producer
 * records already checked positions without rebuilding graphs or owner sets. */
static XrLirStatus lir_plan_collect(const XrLirModule *m, XrLirOwnershipPlan *plan,
    const XrLirCallableFacts *callables, XrLirDiagnostic *d) {
    for (uint32_t function = 1; function <= m->function_count; ++function) {
        const XrLirFunctionData *f = &m->functions[function - 1];
        if (!lir_plan_function(plan,m,f,function)) {
            lir_status_error(d,XR_LIR_INVALID_ARGUMENT,"constructed ownership facts are missing");
            if (d) d->function = function;
            return XR_LIR_INVALID_ARGUMENT;
        }
        if (f->kind == XR_LIR_FUNCTION_EXTERNAL) continue;
        XrLirStatus callable_status = lir_plan_callables(plan,f,function,callables);
        if (callable_status != XR_LIR_OK) return callable_status;
        for (uint32_t p = 0; p < f->parameter_count; ++p)
            if (lir_managed_value(m,f->parameter_types[p]) && !lir_plan_value(plan,f,
                XR_LIR_OWNERSHIP_PARAMETER_BORROW,function,0,0,f->parameters[p],p)) return XR_LIR_OUT_OF_MEMORY;
        for (uint32_t b = 0; b < f->block_count; ++b) {
            const XrLirBlockData *block = &f->blocks[b];
            for (uint32_t p = 0; p < block->parameter_count; ++p)
                if (lir_managed_value(m,f->values[block->parameters[p] - 1].type) && !lir_plan_value(plan,f,
                    XR_LIR_OWNERSHIP_BLOCK_OWNER,function,b + 1,0,block->parameters[p],p)) return XR_LIR_OUT_OF_MEMORY;
            for (uint32_t i = 0; i < block->instruction_count; ++i) {
                const XrLirInstructionView *v = &block->instructions[i].view;
                if (!lir_plan_binding(plan,f,v,function,b + 1,i + 1)) return XR_LIR_OUT_OF_MEMORY;
                XrLirOwnershipFactKind transfer = lir_plan_transfer(m,f,v);
                for (uint32_t a = 0; a < v->operand_count; ++a) {
                    uint32_t id = v->operands[a];
                    if (lir_managed_value(m,f->values[id - 1].type) && !(a == 0 && transfer) &&
                        !lir_plan_value(plan,f,XR_LIR_OWNERSHIP_OPERAND_BORROW,function,b + 1,i + 1,id,a))
                        return XR_LIR_OUT_OF_MEMORY;
                }
                if (transfer && !lir_plan_value(plan,f,transfer,function,b + 1,i + 1,v->operands[0],0))
                    return XR_LIR_OUT_OF_MEMORY;
                if (v->result && lir_managed_value(m,v->type) && !lir_plan_value(plan,f,
                    XR_LIR_OWNERSHIP_RESULT_OWNER,function,b + 1,i + 1,v->result,0)) return XR_LIR_OUT_OF_MEMORY;
                if (v->op == XR_LIR_CALL_INTRINSIC) {
                    const XrLirArrayIntrinsic *shape = xr_lir_internal_array_intrinsic(m,v->intrinsic);
                    if (shape) {
                        bool constructor = shape->operation == XR_LIR_ARRAY_OP_NEW || shape->operation == XR_LIR_ARRAY_OP_REPEAT;
                        uint32_t receiver = constructor ? 0 : v->operands[0];
                        if (!lir_plan_add(plan,(XrLirOwnershipFact){.kind=XR_LIR_OWNERSHIP_ARRAY_OPERATION,
                            .function=function,.block=b + 1,.instruction=i + 1,.value=receiver,
                            .type=receiver ? f->values[receiver - 1].type : XR_LIR_TYPE_INVALID,
                            .array_element=constructor ? shape->result_element : shape->parameter_elements[0],
                            .array_operation=shape->operation})) return XR_LIR_OUT_OF_MEMORY;
                    }
                    XrLirIntrinsic entry;
                    if (xr_lir_intrinsic_view(m,v->intrinsic,&entry) == XR_LIR_OK && xr_lir_map_intrinsic_valid(m,&entry)) {
                        uint32_t receiver=entry.operation == XR_LIR_MAP_OP_NEW ? 0 : v->operands[0];
                        if (!lir_plan_add(plan,(XrLirOwnershipFact){.kind=XR_LIR_OWNERSHIP_MAP_OPERATION,
                            .function=function,.block=b + 1,.instruction=i + 1,.value=receiver,
                            .type=receiver ? f->values[receiver - 1].type : XR_LIR_TYPE_INVALID,
                            .map_operation=(XrLirMapOperation)entry.operation})) return XR_LIR_OUT_OF_MEMORY;
                    }
                }
            }
            const XrLirInstructionView *term = lir_terminator(f,b);
            for (uint32_t e = 0; e < term->edge_count; ++e) {
                const XrLirEdgeView *edge = &term->edges[e];
                for (uint32_t a = 0; a < edge->argument_count; ++a) {
                    uint32_t id = edge->arguments[a];
                    if (lir_managed_value(m,f->values[id - 1].type) &&
                        !lir_plan_add(plan,(XrLirOwnershipFact){.kind=XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER,
                            .function=function,.block=b + 1,.instruction=block->instruction_count,.value=id,
                            .argument=a,.edge=e + 1,.target_block=edge->target,
                            .target_value=f->blocks[edge->target - 1].parameters[a],
                            .type=f->values[id - 1].type,.array_element=f->values[id - 1].array_element,
                            .transfer_mode=XR_LIR_OWNERSHIP_TRANSFER_MOVE}))
                        return XR_LIR_OUT_OF_MEMORY;
                }
            }
        }
    }
    return XR_LIR_OK;
}

XR_FUNC void xr_lir_ownership_plan_destroy(XrLirOwnershipPlan *plan) {
    if (!plan) return;
    xr_free(plan->facts);
    xr_free(plan->types);
    xr_free(plan->functions);
    xr_free(plan);
}

static XrLirStatus lir_plan_receive(const XrLirOwnershipRequest *request,
    const XrLirCallableFacts *callables, XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
    if (d) memset(d, 0, sizeof(*d));
    const XrLirModule *m = request ? request->module : NULL;
    if (!request || !output || *output || !xr_lir_module_is_readable(m) ||
        ((request->type_count != 0) != (request->types != NULL)) || request->type_count > m->type_count ||
        m->function_count > m->function_capacity || (m->function_count && !m->functions))
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid ownership plan request");
    for (uint32_t i = 0; i < request->type_count; ++i) {
        XrLirTypeView view;
        if (xr_lir_type_view(m, request->types[i].type, &view) != XR_LIR_OK ||
            (view.kind != XR_LIR_TYPE_RECORD && view.kind != XR_LIR_TYPE_NULLABLE) ||
            (i && request->types[i - 1].type >= request->types[i].type))
            return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid constructed ownership facts");
    }
    XrLirOwnershipPlan *plan = lir_plan_allocate(1, sizeof(*plan));
    if (!plan) return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "ownership plan allocation failed");
    plan->function_count = m->function_count;
    plan->type_count = request->type_count;
    if (plan->type_count) {
        plan->types = lir_plan_allocate(plan->type_count, sizeof(*plan->types));
        if (!plan->types) {
            xr_lir_ownership_plan_destroy(plan);
            return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "ownership type facts allocation failed");
        }
        memcpy(plan->types, request->types, (size_t)plan->type_count * sizeof(*plan->types));
    }
    if (plan->function_count) {
        plan->functions = lir_plan_allocate(plan->function_count, sizeof(*plan->functions));
        if (!plan->functions) {
            xr_lir_ownership_plan_destroy(plan);
            return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "ownership plan allocation failed");
        }
    }
    XrLirStatus status = lir_plan_collect(m, plan, callables, d);
    if (status != XR_LIR_OK) {
        xr_lir_ownership_plan_destroy(plan);
        if (d && d->status == XR_LIR_OK) lir_status_error(d,status,"ownership event collection failed");
        return status;
    }
    *output = plan;
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_internal_ownership_collect(const XrLirOwnershipRequest *request,
    XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
    return lir_plan_receive(request,request && request->module ? request->module->callable_facts : NULL,output,d);
}
XR_FUNC XrLirStatus xr_lir_ownership_plan_create(const XrLirOwnershipRequest *request,
    XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
    if (d) memset(d,0,sizeof(*d));
    if (!request || !output || *output || !xr_lir_module_is_readable(request->module))
        return lir_status_error(d,XR_LIR_INVALID_ARGUMENT,"invalid ownership plan request");
    XrLirCallableFacts *callables = NULL;
    XrLirStatus status = lir_verify(request->module,d,&callables);
    if (status == XR_LIR_OK) status = lir_plan_receive(request,callables,output,d);
    xr_lir_callable_facts_destroy(callables);
    return status;
}

XR_FUNC uint32_t xr_lir_ownership_function_count(const XrLirOwnershipPlan *plan) {
    return plan ? plan->function_count : 0;
}
XR_FUNC uint32_t xr_lir_ownership_fact_count(const XrLirOwnershipPlan *plan) {
    return plan ? plan->fact_count : 0;
}
XR_FUNC XrLirStatus xr_lir_ownership_function_at(const XrLirOwnershipPlan *plan,
    uint32_t function, XrLirOwnershipFunction *out) {
    if (out) *out = (XrLirOwnershipFunction){0};
    if (!plan || !out || !function || function > plan->function_count) return XR_LIR_INVALID_ARGUMENT;
    *out = plan->functions[function - 1];
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_ownership_fact_at(const XrLirOwnershipPlan *plan,
    uint32_t ordinal, XrLirOwnershipFact *out) {
    if (out) *out = (XrLirOwnershipFact){0};
    if (!plan || !out || ordinal >= plan->fact_count) return XR_LIR_INVALID_ARGUMENT;
    *out = plan->facts[ordinal];
    return XR_LIR_OK;
}
