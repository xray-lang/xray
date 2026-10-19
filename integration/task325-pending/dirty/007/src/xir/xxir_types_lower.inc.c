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
static bool lower_type_work(XrXirCompileContext *budget, uint64_t work) {
    if (!xir_compile_work(budget, work)) return false;
     return true;
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
static XrXirStatus lower_provenance(XrXirModule *module, const uint32_t *map,
    uint32_t count, XrXirCompileContext *budget) {
    XrXirStatus allocation_status = XR_XIR_OK;
    XrXirProvenance *proof = (XrXirProvenance *)module->provenance;
    if (!proof) return XR_XIR_OK;
    if (proof->kind != XR_XIR_EVIDENCE_INSTANCE) return XR_XIR_BAD_STAGE;
    for (uint32_t f = 0; f < proof->count; ++f) {
        XrXirOrigin *origin = &proof->origins[f];
        if (!lower_type_work(budget, (uint64_t)origin->argument_count + 1)) return XR_XIR_BUDGET;
        XrXirType *arguments = (XrXirType *)origin->arguments;
        for (uint32_t a = 0; a < origin->argument_count; ++a)
            if (!lower_type_id(&arguments[a], map, count)) return XR_XIR_BAD_TYPE;
        if (!lower_type_work(budget, origin->effect_argument_count)) return XR_XIR_BUDGET;
        XrXirEffectArgument *effects = (XrXirEffectArgument *)origin->effect_arguments;
        for (uint32_t a = 0; a < origin->effect_argument_count; ++a)
            if (!lower_type_id(&effects[a].type, map, count)) return XR_XIR_BAD_TYPE;
        const XrXirFunction *source = &proof->source->module.functions[origin->function];
        XrXirFunction *function = (XrXirFunction *)&module->functions[f];
        /* Complete checking proved the default advertisement before remapping.
         * Its spelling stays unchanged; variant suffixes use the new type IDs.
         * The final provenance checker independently rebuilds both identities. */
        if (!lower_type_work(budget, 2)) return XR_XIR_BUDGET;
        if (!origin->argument_count && function->name_length == source->name_length) continue;
        uint64_t capacity = (uint64_t)source->name_length + 32 +
            (uint64_t)origin->argument_count * 12 + (uint64_t)origin->effect_argument_count * 26;
        if (capacity > UINT32_MAX || capacity > SIZE_MAX ||
            !lower_type_work(budget, capacity)) return XR_XIR_BUDGET;

        char *name = xir_compile_alloc(budget, (size_t)capacity, &allocation_status);
        if (!name) return allocation_status;
        memcpy(name, source->name, source->name_length);
        size_t at = source->name_length;
        for (uint32_t a = 0; a <= origin->argument_count; ++a) {
            int n = snprintf(name + at, (size_t)capacity - at, a ? ":%u" : "$%u",
                (unsigned)(a ? arguments[a - 1] : origin->function));
            if (n < 0 || (size_t)n >= capacity - at) { xr_compile_resources_free(name); return XR_XIR_BAD_STRUCTURE; }
            at += (size_t)n;
        }
        for (uint32_t a = 0; a < origin->effect_argument_count; ++a) {
            int n = snprintf(name + at, (size_t)capacity - at, "@%u:%u",
                effects[a].parameter, (unsigned)effects[a].type);
            if (n < 0 || (size_t)n >= capacity - at) { xr_compile_resources_free(name); return XR_XIR_BAD_STRUCTURE; }
            at += (size_t)n;
        }
        xr_compile_resources_free((void *)function->name); function->name = name; function->name_length = (uint32_t)at;
    }
    return XR_XIR_OK;
}

static XrXirStatus lower_type_uses(XrXirModule *module, const uint32_t *map, uint32_t count, XrXirCompileContext *budget) {
    XrXirFunction *functions = (XrXirFunction *) module->functions;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirFunction *function = &functions[f];
        if (!lower_type_work(budget, (uint64_t) function->parameter_count + function->instruction_count + 1)) return XR_XIR_BUDGET;
        if (!lower_type_id(&function->result, map, count)) return XR_XIR_BAD_TYPE;
        XrXirType *parameters = (XrXirType *) function->parameters;
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            if (!lower_type_id(&parameters[p], map, count)) return XR_XIR_BAD_TYPE;
        XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (!lower_type_id(&ops[i].type, map, count)) return XR_XIR_BAD_TYPE;
            if (ops[i].op == XR_XIR_ERROR_IS) {
                XrXirType target = (XrXirType) ops[i].immediate;
                if (!lower_type_work(budget, 1)) return XR_XIR_BUDGET;
                if (!lower_type_id(&target, map, count)) return XR_XIR_BAD_TYPE;
                ops[i].immediate = target;
            }
        }
    }
    if (module->declarations) {
        if (!lower_type_work(budget, module->declarations->slot_count)) return XR_XIR_BUDGET;
        XrXirSlot *slots = (XrXirSlot *) module->declarations->slots;
        for (uint32_t s = 0; s < module->declarations->slot_count; ++s)
            if (!lower_type_id(&slots[s].type, map, count)) return XR_XIR_BAD_TYPE;
    }
    return lower_provenance(module, map, count, budget);
}
static XrXirStatus lower_nominal_types(XrXirModule *module, XrXirCompileContext *budget) {
    XrXirStatus allocation_status = XR_XIR_OK;
    XrXirTypes *types = (XrXirTypes *) module->types;
    if (!types || !types->nominals) return XR_XIR_OK;
    const XrXirNominalTable *table = types->nominals;
    if (!table->declarations || table->identities) return XR_XIR_BAD_STAGE;
    uint32_t count = types->count;
    if (!lower_type_work(budget, count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind == XR_XIR_TYPE_NOMINAL && !node->parameter_span &&
            node->nominal.field_count != table->declarations[node->nominal.declaration].field_count) return XR_XIR_BAD_STAGE;
    }
    uint64_t bytes = (uint64_t) count * sizeof(uint32_t);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    uint32_t *map = count ? xir_compile_alloc(budget, (size_t) bytes, &allocation_status) : NULL;
    if (count && !map) return allocation_status;
    uint32_t closed = 0;
    for (uint32_t i = 0; i < count; ++i)
        map[i] = types->nodes[i].parameter_span ? UINT32_MAX : XR_XIR_CONSTRUCTED_TYPE_BASE + closed++;
    XrXirNominalTable *identities = NULL;
    XrXirStatus status = xr_xir_compile_nominal_project(budget, table, &identities);
    if (status != XR_XIR_OK) { xr_compile_resources_free(map); return status; }
    xr_xir_compile_nominal_free((XrXirNominalTable *) table); types->nominals = identities;
    XrXirTypeNode *nodes = (XrXirTypeNode *) types->nodes;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirTypeNode *node = &nodes[i];
        if (map[i] == UINT32_MAX) {
            if (!lower_type_work(budget, sizeof(*node))) { status = XR_XIR_BUDGET; break; }
            xr_compile_resources_free((void *) node->parameters); xr_compile_resources_free((void *) node->nominal.arguments); xr_compile_resources_free((void *) node->nominal.fields);
            memset(node, 0, sizeof(*node)); continue;
        }
        if (!lower_type_work(budget, (uint64_t) node->parameter_count + node->nominal.argument_count + node->nominal.field_count + 1)) {
            status = XR_XIR_BUDGET; break;
        }
        if (!lower_type_node(node, map, count)) { status = XR_XIR_BAD_TYPE; break; }
        uint32_t target = map[i] - XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (target != i) {
            if (!lower_type_work(budget, sizeof(*node))) { status = XR_XIR_BUDGET; break; }
            nodes[target] = *node; memset(node, 0, sizeof(*node));
        }
    }
    if (status == XR_XIR_OK) {
        types->count = closed;
        if (!closed) { xr_compile_resources_free(nodes); types->nodes = NULL; }
        status = lower_type_uses(module, map, count, budget);
    }
    xr_compile_resources_free(map); return status;
}
