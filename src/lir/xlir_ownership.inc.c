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
    return lir_plan_add(plan, (XrLirOwnershipFact){.kind=kind, .function=function,
        .block=block, .instruction=instruction, .value=value, .argument=argument,
        .type=f->values[value - 1].type, .array_element=f->values[value - 1].array_element});
}

/* Both lifetime checking and event collection consume this transfer identity. */
static XrLirOwnershipFactKind lir_plan_transfer(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    if (v->op == XR_LIR_RET && v->operand_count && lir_managed_value(m,f->result))
        return XR_LIR_OWNERSHIP_RETURN_TRANSFER;
    if (xr_lir_string_role(m,v) == XR_LIR_STRING_OP_APPEND_OWNED) return XR_LIR_OWNERSHIP_APPEND_TRANSFER;
    if (v->op == XR_LIR_STRING_DROP || v->op == XR_LIR_ARRAY_DROP ||
        v->op == XR_LIR_VALUE_DROP || v->op == XR_LIR_MAP_DROP) return XR_LIR_OWNERSHIP_RELEASE;
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
/* The verifier establishes ownership once. This one deterministic producer
 * records already checked positions without rebuilding graphs or owner sets. */
static XrLirStatus lir_plan_collect(const XrLirModule *m, XrLirOwnershipPlan *plan, XrLirDiagnostic *d) {
    for (uint32_t function = 1; function <= m->function_count; ++function) {
        const XrLirFunctionData *f = &m->functions[function - 1];
        if (!lir_plan_function(plan,m,f,function)) {
            lir_status_error(d,XR_LIR_INVALID_ARGUMENT,"constructed ownership facts are missing");
            if (d) d->function = function;
            return XR_LIR_INVALID_ARGUMENT;
        }
        if (f->kind == XR_LIR_FUNCTION_EXTERNAL) continue;
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
                            .type=f->values[id - 1].type,.array_element=f->values[id - 1].array_element}))
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

XR_FUNC XrLirStatus xr_lir_internal_ownership_collect(const XrLirOwnershipRequest *request,
    XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
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
    XrLirStatus status = lir_plan_collect(m, plan, d);
    if (status != XR_LIR_OK) {
        xr_lir_ownership_plan_destroy(plan);
        if (d && d->status == XR_LIR_OK) lir_status_error(d,status,"ownership event collection failed");
        return status;
    }
    *output = plan;
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_ownership_plan_create(const XrLirOwnershipRequest *request,
    XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
    if (d) memset(d,0,sizeof(*d));
    if (!request || !output || *output || !xr_lir_module_is_readable(request->module))
        return lir_status_error(d,XR_LIR_INVALID_ARGUMENT,"invalid ownership plan request");
    XrLirStatus status = lir_verify(request->module,d,NULL);
    return status == XR_LIR_OK ? xr_lir_internal_ownership_collect(request,output,d) : status;
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
