/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_types_lower.inc.c - Remove template expressions from owned Lowered types
 *
 * KEY CONCEPT:
 *   Compaction remaps all consumers before the artifact is published.
 */
static bool lower_type_work(XrXirBudget *budget, uint64_t work) {
    if (work > budget->work) return false;
    budget->work -= work; return true;
}
static bool lower_type_id(XrXirType *type, const uint32_t *map, uint32_t count) {
    uint32_t id = (uint32_t) *type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) return false;
    if (id < XR_XIR_CONSTRUCTED_TYPE_BASE || id >= XR_XIR_CONSTRUCTED_TYPE_LIMIT) return true;
    uint32_t index = id - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (index >= count || map[index] == UINT32_MAX) return false;
    *type = (XrXirType) map[index]; return true;
}
static bool lower_type_node(XrXirTypeNode *node, const uint32_t *map, uint32_t count) {
    if (!lower_type_id(&node->element, map, count) || !lower_type_id(&node->result, map, count)) return false;
    XrXirCallableParameter *parameters = (XrXirCallableParameter *) node->parameters;
    for (uint32_t p = 0; p < node->parameter_count; ++p)
        if (!lower_type_id(&parameters[p].type, map, count)) return false;
    XrXirType *arguments = (XrXirType *) node->nominal.arguments, *fields = (XrXirType *) node->nominal.fields;
    for (uint32_t p = 0; p < node->nominal.argument_count; ++p)
        if (!lower_type_id(&arguments[p], map, count)) return false;
    for (uint32_t f = 0; f < node->nominal.field_count; ++f)
        if (!lower_type_id(&fields[f], map, count)) return false;
    return true;
}
static XrXirStatus lower_type_uses(XrXirModule *module, const uint32_t *map, uint32_t count, XrXirBudget *budget) {
    XrXirFunction *functions = (XrXirFunction *) module->functions;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirFunction *function = &functions[f];
        if (!lower_type_work(budget, (uint64_t) function->parameter_count + function->instruction_count + 1)) return XR_XIR_BUDGET;
        if (!lower_type_id(&function->result, map, count)) return XR_XIR_BAD_TYPE;
        XrXirType *parameters = (XrXirType *) function->parameters;
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            if (!lower_type_id(&parameters[p], map, count)) return XR_XIR_BAD_TYPE;
        XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            if (!lower_type_id(&ops[i].type, map, count)) return XR_XIR_BAD_TYPE;
    }
    if (module->declarations) {
        if (!lower_type_work(budget, module->declarations->slot_count)) return XR_XIR_BUDGET;
        XrXirSlot *slots = (XrXirSlot *) module->declarations->slots;
        for (uint32_t s = 0; s < module->declarations->slot_count; ++s)
            if (!lower_type_id(&slots[s].type, map, count)) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus lower_nominal_types(XrXirModule *module, XrXirBudget *budget) {
    XrXirTypes *types = (XrXirTypes *) module->types;
    if (!types || !types->nominals) return XR_XIR_OK;
    const XrXirNominalTable *table = types->nominals;
    if (!table->declarations || table->identities) return XR_XIR_BAD_STAGE;
    uint32_t count = types->count;
    if (!lower_type_work(budget, count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind == XR_XIR_TYPE_NOMINAL &&
            node->nominal.field_count != table->declarations[node->nominal.declaration].field_count) return XR_XIR_BAD_STAGE;
    }
    uint64_t bytes = (uint64_t) count * sizeof(uint32_t);
    if (bytes > SIZE_MAX || bytes > budget->metadata_bytes) return XR_XIR_BUDGET;
    budget->metadata_bytes -= bytes;
    uint32_t *map = count ? xr_malloc((size_t) bytes) : NULL;
    if (count && !map) return XR_XIR_OUT_OF_MEMORY;
    uint32_t closed = 0;
    for (uint32_t i = 0; i < count; ++i)
        map[i] = types->nodes[i].parameter_span ? UINT32_MAX : XR_XIR_CONSTRUCTED_TYPE_BASE + closed++;
    XrXirNominalTable *identities = NULL;
    XrXirStatus status = xr_xir_nominal_project(table, budget, &identities);
    if (status != XR_XIR_OK) { xr_free(map); return status; }
    xr_xir_nominal_free((XrXirNominalTable *) table); types->nominals = identities;
    XrXirTypeNode *nodes = (XrXirTypeNode *) types->nodes;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirTypeNode *node = &nodes[i];
        if (map[i] == UINT32_MAX) {
            xr_free((void *) node->parameters); xr_free((void *) node->nominal.arguments); xr_free((void *) node->nominal.fields);
            memset(node, 0, sizeof(*node)); continue;
        }
        if (!lower_type_work(budget, (uint64_t) node->parameter_count + node->nominal.argument_count + node->nominal.field_count + 1)) {
            status = XR_XIR_BUDGET; break;
        }
        if (!lower_type_node(node, map, count)) { status = XR_XIR_BAD_TYPE; break; }
        uint32_t target = map[i] - XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (target != i) { nodes[target] = *node; memset(node, 0, sizeof(*node)); }
    }
    if (status == XR_XIR_OK) {
        types->count = closed;
        if (!closed) { xr_free(nodes); types->nodes = NULL; }
        status = lower_type_uses(module, map, count, budget);
    }
    xr_free(map); return status;
}
