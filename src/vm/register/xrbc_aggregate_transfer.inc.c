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
    if (state != RBC_STATE_REFERENCE || !origin ||
        !rbc_aggregate_state_type(s->module, value.type, (origin >> 1) - 1u)) return false;
    if (!read) return (origin & 1u) && s->aggregates[home];
    s->aggregates[home] = 1;
    return true;
}

static bool rbc_nullable_transfer(RbcStringAdmission *s, XrRbcAggregateAccess access) {
    bool create = access.operation == XR_RBC_NULLABLE_NONE || access.operation == XR_RBC_NULLABLE_SOME;
    uint32_t home = create ? access.destination : access.source;
    XrPodType type = s->module->aggregate_types[s->function->aggregate_values[home].type];
    if (!create && !s->aggregates[home]) return false;
    if (access.operation == XR_RBC_NULLABLE_NONE) { s->aggregates[home] = 1; return true; }
    if (access.operation != XR_RBC_NULLABLE_IS_SOME && type.element >= XR_POD_SCALAR_COUNT) {
        if (create && !s->aggregates[access.source]) return false;
        s->aggregates[access.destination] = 1;
        return true;
    }
    if (access.operation != XR_RBC_NULLABLE_IS_SOME && type.element == XR_POD_UNIT) {
        if (create) s->aggregates[home] = 1;
        return true;
    }
    XrRbcArrayLocation location = access.result;
    if (create) {
        if ((location.spill ? s->spills[location.index].state : s->state[location.index]) != RBC_BITS) return false;
        s->aggregates[home] = 1;
    } else if (location.spill) s->spills[location.index] = (RbcSpillState){0, RBC_BITS};
    else {
        if (!rbc_bits_destination(s, location.index)) return false;
        s->state[location.index] = RBC_BITS;
    }
    return true;
}

#include "xrbc_string_library_transfer.inc.c"

static bool rbc_aggregate_transfer(RbcStringAdmission *s, uint32_t index) {
    XrRbcAggregateAccess access = s->function->aggregate_accesses[index];
    if (access.operation == XR_RBC_RANGE_CHECK) {
        for (uint32_t p = 0; p < 3; ++p)
            if (rbc_array_state(s, s->function->aggregate_operands[access.first + p]) != RBC_BITS) return false;
        return true;
    }
    if (access.operation == XR_RBC_FUNCTION_RETURN)
        return rbc_string_step(s, (XrRbcOperands){.op=access.count ? XR_RBC_RET : XR_RBC_RET0, .a=access.source});
    if (access.operation == XR_RBC_FUNCTION_REFERENCE) {
        uint32_t r = access.result.index;
        if (s->state[r] != RBC_EMPTY && s->state[r] != RBC_FUNCTION) return false;
        s->state[r] = RBC_FUNCTION;
        s->origin[r] = access.slot;
        return true;
    }
    if (access.operation == XR_RBC_FUNCTION_CALL) {
        if (s->state[access.source] != RBC_FUNCTION || s->origin[access.source] != access.slot) return false;
        XrRbcCallableType type = s->module->callable_types[access.slot - 1];
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            uint32_t r = access.window + 1 + p;
            XrRbcCallableValue value = type.parameters[p].value;
            if (value.kind == XR_RBC_CALLABLE_FUNCTION) {
                if (s->state[r] != RBC_FUNCTION || s->origin[r] != value.type + 1) return false;
            } else if (!rbc_call_parameter_matches(s, r, rbc_callable_value_kind(value))) return false;
        }
        if (!rbc_bits_destination(s, access.window)) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) s->state[access.window + 1 + p] = RBC_EMPTY;
        XrRbcRegisterKind result = rbc_callable_value_kind(type.result);
        s->state[access.window] = rbc_managed_kind(result) ? RBC_STRING_OWNED : RBC_BITS;
        s->origin[access.window] = result;
        return true;
    }
    if (access.operation == XR_RBC_STRING_LIBRARY) {
        return rbc_string_library_transfer(s, access);
    }
    if (access.operation == XR_RBC_AGGREGATE_DISPLAY) {
        if (!rbc_array_destination(s, access.result)) return false;
        if (access.slot >= XR_POD_SCALAR_COUNT) {
            if (!s->aggregates[access.source]) return false;
        } else if (rbc_array_state(s, s->function->aggregate_operands[access.first]) != RBC_BITS) return false;
        rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
        return true;
    }
    if (access.operation >= XR_RBC_NULLABLE_NONE && access.operation <= XR_RBC_NULLABLE_UNWRAP)
        return rbc_nullable_transfer(s, access);
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
