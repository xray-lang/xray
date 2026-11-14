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
};

static XrLirStatus lir_verify(const XrLirModule *, XrLirDiagnostic *, XrLirOwnershipPlan *);

static void *lir_plan_allocate(size_t count, size_t size) {
    return size && count && count <= SIZE_MAX / size ? xr_calloc(count, size) : NULL;
}

static uint32_t lir_plan_unmodeled(XrLirType type) {
    switch (type) {
        case XR_LIR_I64_CELL: case XR_LIR_STRING_CELL: case XR_LIR_ARRAY_CELL:
        case XR_LIR_MAP_STRING_I64_CELL: return XR_LIR_OWNERSHIP_UNMODELED_CELL;
        case XR_LIR_STATE_REF: case XR_LIR_ARRAY_REF: case XR_LIR_STRING_REF:
        case XR_LIR_MAP_STRING_I64_REF: return XR_LIR_OWNERSHIP_UNMODELED_REFERENCE;
        case XR_LIR_STATE: return XR_LIR_OWNERSHIP_UNMODELED_STATE;
        case XR_LIR_VIEW_FRAME: case XR_LIR_VIEW_CELL: case XR_LIR_SLICE_I64:
            return XR_LIR_OWNERSHIP_UNMODELED_VIEW;
        case XR_LIR_FUNCTION_SCOPE: return XR_LIR_OWNERSHIP_UNMODELED_CALLABLE;
        default: return type >= XR_LIR_TYPE_COUNT ? XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED : 0;
    }
}

static void lir_plan_function(XrLirOwnershipPlan *plan, const XrLirFunctionData *f,
    uint32_t function) {
    if (!plan) return;
    XrLirOwnershipFunction *out = &plan->functions[function - 1];
    out->fact_offset = plan->fact_count;
    out->external = f->kind == XR_LIR_FUNCTION_EXTERNAL;
    out->unmodeled = lir_plan_unmodeled(f->result);
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        out->unmodeled |= lir_plan_unmodeled(f->parameter_types[p]);
    for (uint32_t v = 0; v < f->value_count; ++v)
        out->unmodeled |= lir_plan_unmodeled(f->values[v].type);
}

static bool lir_plan_add(XrLirOwnershipPlan *plan, XrLirOwnershipFact fact) {
    if (!plan) return true;
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

XR_FUNC void xr_lir_ownership_plan_destroy(XrLirOwnershipPlan *plan) {
    if (!plan) return;
    xr_free(plan->facts);
    xr_free(plan->functions);
    xr_free(plan);
}

XR_FUNC XrLirStatus xr_lir_ownership_plan_create(const XrLirModule *m,
    XrLirOwnershipPlan **output, XrLirDiagnostic *d) {
    if (d) memset(d, 0, sizeof(*d));
    if (!output || *output || !xr_lir_module_is_readable(m) ||
        m->function_count > m->function_capacity || (m->function_count && !m->functions))
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid ownership plan request");
    XrLirOwnershipPlan *plan = lir_plan_allocate(1, sizeof(*plan));
    if (!plan) return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "ownership plan allocation failed");
    plan->function_count = m->function_count;
    if (plan->function_count) {
        plan->functions = lir_plan_allocate(plan->function_count, sizeof(*plan->functions));
        if (!plan->functions) {
            xr_lir_ownership_plan_destroy(plan);
            return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "ownership plan allocation failed");
        }
    }
    XrLirStatus status = lir_verify(m, d, plan);
    if (status != XR_LIR_OK) {
        xr_lir_ownership_plan_destroy(plan);
        return status;
    }
    *output = plan;
    return XR_LIR_OK;
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
