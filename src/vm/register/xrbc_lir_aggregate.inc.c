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

static bool rbc_lir_fixed_state(const XrLirModule *module, XrLirFunction function,
    const XrLirInstructionView *instruction) {
    if (instruction->op == XR_LIR_STATE_READ) return instruction->type >= XR_LIR_TYPE_COUNT;
    if ((instruction->op != XR_LIR_STATE_WRITE && instruction->op != XR_LIR_STATE_CONSTRUCT &&
         instruction->op != XR_LIR_STATE_INITIALIZE) || instruction->operand_count != 2) return false;
    XrLirType type = XR_LIR_TYPE_INVALID;
    xr_lir_value_type(module, (XrLirValue){module, function.id, instruction->operands[1]}, &type);
    return type >= XR_LIR_TYPE_COUNT;
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
    uint32_t parameters = 0;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            XrLirType type = XR_LIR_TYPE_INVALID;
            xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, view.parameters[p]}, &type);
            parameters += type >= XR_LIR_TYPE_COUNT;
        }
    }
    size_t values = (size_t)c->view.value_count + 1;
    if (parameters > UINT32_MAX - count || values > SIZE_MAX / sizeof(*c->aggregate_home) ||
        sizeof(*c->aggregate_values) > SIZE_MAX / ((size_t)count + parameters))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate home metadata exceeds address space");
    c->aggregate_home = xr_malloc(values * sizeof(*c->aggregate_home));
    c->aggregate_scratch = parameters ? xr_malloc(values * sizeof(*c->aggregate_scratch)) : NULL;
    c->aggregate_values = xr_malloc(((size_t)count + parameters) * sizeof(*c->aggregate_values));
    if (!c->aggregate_home || !c->aggregate_values || (parameters && !c->aggregate_scratch))
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
    for (uint32_t b = 0; parameters && b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            uint32_t value = view.parameters[p];
            if (c->aggregate_home[value] == UINT32_MAX) continue;
            XrRbcAggregateValue original = c->aggregate_values[c->aggregate_home[value]];
            XrRbcAggregateLayout layout;
            xr_rbc_aggregate_layout(module->aggregate_types[original.type], &layout);
            uint64_t offset = ((uint64_t)c->aggregate_size + layout.alignment - 1) & ~(uint64_t)(layout.alignment - 1);
            if (offset + layout.size > UINT32_MAX)
                return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate edge storage exceeds indexed capacity");
            c->aggregate_scratch[value] = c->aggregate_value_count;
            c->aggregate_values[c->aggregate_value_count++] = (XrRbcAggregateValue){original.type, (uint32_t)offset};
            c->aggregate_size = (size_t)offset + layout.size;
        }
    }
    return true;
}

static bool rbc_lir_aggregate_append(RbcLirCompiler *c, XrRbcAggregateAccess access) {
    if (c->aggregate_access_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate operations exceed bytecode indices");
    void *storage = c->aggregate_accesses;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_access_capacity,
        c->aggregate_access_count + 1, sizeof(*c->aggregate_accesses))) return false;
    c->aggregate_accesses = storage;
    c->aggregate_accesses[c->aggregate_access_count] = access;
    return rbc_lir_emit(c, XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, (int64_t)c->aggregate_access_count++);
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
    access.first = (uint32_t)c->aggregate_operand_count;
    for (uint32_t n = 0; n < access.count; ++n) {
        uint32_t value = instruction->operands[construct ? n : 1];
        c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, value);
    }
    return rbc_lir_aggregate_append(c, access);
}

static bool rbc_lir_aggregate_state_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    bool read = instruction->op == XR_LIR_STATE_READ;
    bool construct = instruction->op == XR_LIR_STATE_CONSTRUCT;
    XrRbcAggregateAccess access = {.operation = read ? XR_RBC_AGGREGATE_STATE_READ :
        construct ? XR_RBC_AGGREGATE_STATE_CONSTRUCT : instruction->op == XR_LIR_STATE_INITIALIZE ?
        XR_RBC_AGGREGATE_STATE_INITIALIZE : XR_RBC_AGGREGATE_STATE_WRITE, .count = 1};
    if (read) access.destination = c->aggregate_home[instruction->result];
    else access.source = c->aggregate_home[instruction->operands[1]];
    if (construct) access.result = rbc_lir_array_location(c, instruction->result);
    if (construct || instruction->op == XR_LIR_STATE_INITIALIZE) access.slot = (uint32_t)instruction->bits;
    if (c->aggregate_operand_count == UINT32_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate state operands exceed bytecode indices");
    void *storage = c->aggregate_operands;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_operand_capacity,
        c->aggregate_operand_count + 1, sizeof(*c->aggregate_operands))) return false;
    c->aggregate_operands = storage;
    access.first = (uint32_t)c->aggregate_operand_count;
    c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, instruction->operands[0]);
    return rbc_lir_aggregate_append(c, access);
}

static bool rbc_lir_aggregate_edge(RbcLirCompiler *c, XrLirEdgeView edge) {
    if (!c->aggregate_scratch) return true;
    XrLirBlockView target;
    xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, edge.target}, &target);
    /* Every old source reaches a distinct scratch home before any destination
     * is overwritten, including cycles through loop block parameters. */
    for (uint32_t pass = 0; pass < 2; ++pass)
        for (uint32_t p = 0; p < edge.argument_count; ++p) {
            uint32_t actual = edge.arguments[p], formal = target.parameters[p];
            if (c->aggregate_home[actual] == UINT32_MAX) continue;
            uint32_t temporary = c->aggregate_scratch[formal];
            XrRbcAggregateAccess access = {.operation = XR_RBC_AGGREGATE_COPY,
                .source = pass ? temporary : c->aggregate_home[actual],
                .destination = pass ? c->aggregate_home[formal] : temporary};
            if (!rbc_lir_aggregate_append(c, access)) return false;
        }
    return true;
}
