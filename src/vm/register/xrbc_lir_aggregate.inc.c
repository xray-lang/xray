/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_aggregate.inc.c - Owned physical homes and real typed operand spans
 */
static bool rbc_lir_fixed_point(const XrLirInstructionView *instruction) {
    return instruction->op == XR_LIR_FIXED_ARRAY_NEW || instruction->op == XR_LIR_FIXED_ARRAY_GET;
}

static bool rbc_lir_fixed_type(const XrLirModule *module, XrLirType type) {
    XrLirTypeView shape = {0};
    return type >= XR_LIR_TYPE_COUNT && xr_lir_type_view(module, type, &shape) == XR_LIR_OK &&
        shape.kind == XR_LIR_TYPE_FIXED_ARRAY && shape.element == XR_LIR_U8 && shape.count && shape.count <= 65535;
}

static bool rbc_lir_aggregate_homes(RbcLirCompiler *c) {
    if (!xr_lir_constructed_type_count(c->input)) return true;
    uint32_t count = 0;
    for (uint32_t id = 1; id <= c->view.value_count; ++id) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, id}, &type);
        count += type >= XR_LIR_TYPE_COUNT;
    }
    if (!count) return true;
    size_t values = (size_t)c->view.value_count + 1;
    if (values > SIZE_MAX / sizeof(*c->aggregate_home) || sizeof(*c->aggregate_values) > SIZE_MAX / count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate home metadata exceeds address space");
    c->aggregate_home = xr_malloc(values * sizeof(*c->aggregate_home));
    c->aggregate_values = xr_malloc((size_t)count * sizeof(*c->aggregate_values));
    if (!c->aggregate_home || !c->aggregate_values)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own physical aggregate homes");
    for (size_t id = 0; id < values; ++id) c->aggregate_home[id] = UINT32_MAX;
    const XrRbcModule *module = &c->literals->program->module;
    for (uint32_t id = 1; id <= c->view.value_count; ++id) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, id}, &type);
        if (type < XR_LIR_TYPE_COUNT) continue;
        uint32_t ordinal = type - XR_LIR_TYPE_COUNT;
        XrRbcAggregateLayout layout;
        if (ordinal >= module->aggregate_type_count ||
            !xr_rbc_aggregate_layout(module->aggregate_types[ordinal], &layout))
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "aggregate value lost its exact physical type");
        uint64_t offset = ((uint64_t)c->aggregate_size + layout.alignment - 1) & ~(uint64_t)(layout.alignment - 1);
        if (offset + layout.size > UINT32_MAX)
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate activation storage exceeds indexed capacity");
        c->aggregate_home[id] = c->aggregate_value_count;
        c->aggregate_values[c->aggregate_value_count++] = (XrRbcAggregateValue){ordinal, (uint32_t)offset};
        c->aggregate_size = (size_t)offset + layout.size;
    }
    return true;
}

static bool rbc_lir_aggregate_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    bool construct = instruction->op == XR_LIR_FIXED_ARRAY_NEW;
    XrRbcAggregateAccess access = {.operation = construct ? XR_RBC_AGGREGATE_NEW : XR_RBC_AGGREGATE_GET,
        .count = construct ? instruction->operand_count : 1};
    if (construct) access.destination = c->aggregate_home[instruction->result];
    else {
        access.source = c->aggregate_home[instruction->operands[0]];
        access.result = rbc_lir_array_location(c, instruction->result);
    }
    if (c->aggregate_access_count > UINT16_MAX || c->aggregate_operand_count > UINT32_MAX - access.count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate operations exceed bytecode indices");
    void *storage = c->aggregate_operands;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_operand_capacity,
        c->aggregate_operand_count + access.count, sizeof(*c->aggregate_operands))) return false;
    c->aggregate_operands = storage;
    storage = c->aggregate_accesses;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_access_capacity,
        c->aggregate_access_count + 1, sizeof(*c->aggregate_accesses))) return false;
    c->aggregate_accesses = storage;
    access.first = (uint32_t)c->aggregate_operand_count;
    for (uint32_t n = 0; n < access.count; ++n) {
        uint32_t value = instruction->operands[construct ? n : 1];
        c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, value);
    }
    c->aggregate_accesses[c->aggregate_access_count] = access;
    return rbc_lir_emit(c, XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, (int64_t)c->aggregate_access_count++);
}
