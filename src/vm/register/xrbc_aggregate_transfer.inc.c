/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_transfer.inc.c - Definite aggregate initialization on real CFG edges
 */
static bool rbc_aggregate_state_transfer(RbcStringAdmission *s, XrRbcAggregateAccess access) {
    XrRbcArrayLocation location = s->function->aggregate_operands[access.first];
    uint32_t origin = location.spill ? s->spills[location.index].origin : s->origin[location.index];
    uint8_t state = location.spill ? s->spills[location.index].state : s->state[location.index];
    bool read = access.operation == XR_RBC_AGGREGATE_STATE_READ;
    bool construct = access.operation == XR_RBC_AGGREGATE_STATE_CONSTRUCT;
    bool initialize = access.operation == XR_RBC_AGGREGATE_STATE_INITIALIZE;
    uint32_t home = read ? access.destination : access.source;
    XrRbcAggregateValue value = s->function->aggregate_values[home];
    uint32_t count = s->module->aggregate_types[value.type].count;
    if (construct || initialize) {
        if (state != RBC_STATE_BORROWED || !s->aggregates[home]) return false;
        if (construct) {
            origin = xr_rbc_state_shape(s->module, access.slot);
            if (access.result.spill) s->spills[access.result.index] = (RbcSpillState){origin, RBC_STATE_REFERENCE};
            else {
                s->state[access.result.index] = RBC_STATE_REFERENCE;
                s->origin[access.result.index] = origin;
            }
        }
        return true;
    }
    if (state != RBC_STATE_REFERENCE || (origin & 255u) != XRT_MODULE_FIXED_U8 + 1u ||
        (origin >> 16) != count) return false;
    if (!read) return (origin & 256u) && s->aggregates[home];
    s->aggregates[home] = 1;
    return true;
}

static bool rbc_aggregate_transfer(RbcStringAdmission *s, uint32_t index) {
    XrRbcAggregateAccess access = s->function->aggregate_accesses[index];
    if (access.operation == XR_RBC_AGGREGATE_CALL) {
        for (uint32_t n = 0; n < access.count; ++n)
            if (!s->aggregates[s->function->aggregate_arguments[access.first + n]]) return false;
        if (!rbc_strings_call(s, (XrRbcOperands){.op = XR_RBC_CALL, .a = access.window,
            .immediate = access.callee})) return false;
        if (s->module->functions[access.callee].aggregate_result_type) {
            s->aggregates[access.destination] = 1;
            s->state[access.window] = RBC_EMPTY;
        }
        return true;
    }
    if (access.operation == XR_RBC_AGGREGATE_RETURN) {
        if (!s->aggregates[access.source]) return false;
        for (uint32_t r = 0; r < s->function->register_count; ++r)
            if (s->state[r] == RBC_STRING_OWNED || s->state[r] == RBC_CELL_OWNED) return false;
        return true;
    }
    if (access.operation >= XR_RBC_AGGREGATE_STATE_READ)
        return rbc_aggregate_state_transfer(s, access);
    for (uint32_t n = 0; n < access.count; ++n) {
        XrRbcArrayLocation operand = s->function->aggregate_operands[access.first + n];
        if ((operand.spill ? s->spills[operand.index].state : s->state[operand.index]) != RBC_BITS) return false;
    }
    if (access.operation == XR_RBC_AGGREGATE_NEW) {
        s->aggregates[access.destination] = 1;
        return true;
    }
    if (!s->aggregates[access.source]) return false;
    if (access.operation == XR_RBC_AGGREGATE_COPY) {
        s->aggregates[access.destination] = 1;
    } else if (access.result.spill) {
        s->spills[access.result.index] = (RbcSpillState){0, RBC_BITS};
    } else {
        if (!rbc_bits_destination(s, access.result.index)) return false;
        s->state[access.result.index] = RBC_BITS;
    }
    return true;
}
