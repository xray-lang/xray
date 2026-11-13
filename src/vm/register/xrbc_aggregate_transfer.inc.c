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
    if (access.operation >= XR_RBC_CAPTURED_CELL_NEW && access.operation <= XR_RBC_CAPTURED_CELL_BIND) {
        if (access.operation == XR_RBC_CAPTURED_CELL_NEW) {
            if (s->state[access.source] != RBC_BITS) return false;
        } else if (s->state[access.source] != RBC_CAPTURED_CELL || s->origin[access.source] != access.slot) return false;
        if (access.operation == XR_RBC_CAPTURED_CELL_WRITE) return s->state[access.window] == RBC_BITS;
        uint32_t result = access.result.index;
        if (access.operation == XR_RBC_CAPTURED_CELL_READ) {
            if (!rbc_bits_destination(s, result)) return false;
            s->state[result] = RBC_BITS;
            return true;
        }
        if (s->state[result] != RBC_EMPTY && s->state[result] != RBC_CAPTURED_CELL) return false;
        uint32_t scope = rbc_scope_current(s);
        if (access.operation == XR_RBC_CAPTURED_CELL_BIND) {
            if (!rbc_scope_contains(rbc_function_scopes(s->module, s->function), access.destination, scope)) return false;
            scope = access.destination;
        }
        s->state[result] = RBC_CAPTURED_CELL;
        s->origin[result] = access.slot;
        rbc_scope_define(s, result, scope);
        return true;
    }
    if (access.operation >= XR_RBC_FUNCTION_STATE_READ && access.operation <= XR_RBC_FUNCTION_STATE_INITIALIZE) {
        bool read = access.operation == XR_RBC_FUNCTION_STATE_READ;
        bool construct = access.operation == XR_RBC_FUNCTION_STATE_CONSTRUCT;
        bool initialize = access.operation == XR_RBC_FUNCTION_STATE_INITIALIZE;
        uint32_t shape;
        if (construct || initialize) {
            if (s->state[access.source] != RBC_STATE_BORROWED) return false;
            shape = xr_rbc_state_shape(s->module, access.destination);
        } else {
            if (s->state[access.source] != RBC_STATE_REFERENCE) return false;
            shape = s->origin[access.source];
        }
        if (!shape || rbc_callable_state_type(s->module, (shape >> 1) - 1) != access.slot) return false;
        if (read) {
            uint32_t result = access.result.index;
            if (s->state[result] != RBC_EMPTY && !rbc_function_state(s->state[result])) return false;
            s->state[result] = RBC_FUNCTION_OWNED;
            s->origin[result] = access.slot;
            rbc_scope_define(s, result, rbc_scope_current(s));
            return true;
        }
        if ((!initialize && !(shape & 1u)) || s->state[access.window] != RBC_FUNCTION_OWNED ||
            !rbc_callable_accepts(s->module, access.slot, s->origin[access.window])) return false;
        if (construct) {
            s->state[access.result.index] = RBC_STATE_REFERENCE;
            s->origin[access.result.index] = shape;
        }
        return true;
    }
    if (access.operation >= XR_RBC_FUNCTION_SCOPE_ENTER && access.operation <= XR_RBC_FUNCTION_BIND)
        return rbc_callable_scope_transfer(s, access);
    if (access.operation == XR_RBC_RANGE_CHECK) {
        for (uint32_t p = 0; p < 3; ++p)
            if (rbc_array_state(s, s->function->aggregate_operands[access.first + p]) != RBC_BITS) return false;
        return true;
    }
    if (access.operation == XR_RBC_FUNCTION_RETURN || access.operation == XR_RBC_FUNCTION_SCOPE_RETURN)
        return rbc_string_step(s, (XrRbcOperands){.op=access.count ? XR_RBC_RET : XR_RBC_RET0, .a=access.source});
    if (access.operation == XR_RBC_FUNCTION_REFERENCE || access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) {
        uint32_t frame_scope = 0;
        if (access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) {
            for (uint32_t p = 0; p < access.count; ++p) {
                XrRbcArrayLocation location = s->function->aggregate_operands[access.first + p];
                XrRbcCallableValue capture = s->module->functions[access.callee].captures[p];
                if (capture.kind == XR_RBC_CALLABLE_CELL) {
                    if (location.spill || s->state[location.index] != RBC_CAPTURED_CELL ||
                        s->origin[location.index] != capture.type) return false;
                } else if (rbc_array_state(s, location) != RBC_BITS) return false;
            }
        } else if (access.destination) {
            size_t home = s->function->aggregate_value_count + access.destination - 1;
            if (s->aggregates[home]) return false;
            for (uint32_t p = 0; p < access.count; ++p) {
                XrRbcArrayLocation location = s->function->aggregate_operands[access.first + p];
                uint8_t state = rbc_array_state(s, location);
                XrRbcRegisterKind kind = rbc_array_location_kind(s->function, location);
                if (kind == XR_RBC_CAPTURED_I64_CELL || kind == XR_RBC_CAPTURED_I64_BORROW) {
                    if (location.spill || state != RBC_CAPTURED_CELL ||
                        s->origin[location.index] != s->module->functions[access.callee].captures[p].type) return false;
                    const uint32_t *scopes = rbc_scope_state(s);
                    uint32_t owner = scopes ? scopes[location.index + 1] : 0;
                    XrRbcFunctionScopes shape = rbc_function_scopes(s->module, s->function);
                    if (rbc_scope_contains(shape, frame_scope, owner)) frame_scope = owner;
                    else if (!rbc_scope_contains(shape, owner, frame_scope)) return false;
                    continue;
                }
                if (kind == XR_RBC_STATE ? state != RBC_STATE_BORROWED :
                    kind == XR_RBC_I64_BINDING ? state != RBC_CELL_BORROWED : state != RBC_BITS) return false;
            }
            s->aggregates[home] = 1;
        }
        uint32_t r = access.result.index;
        if (s->state[r] != RBC_EMPTY && !rbc_function_state(s->state[r])) return false;
        s->state[r] = access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE || !access.destination ?
            RBC_FUNCTION_OWNED : RBC_FUNCTION;
        s->origin[r] = access.slot;
        rbc_scope_define(s, r, s->state[r] == RBC_FUNCTION && rbc_scope_captured_cells(s->function) ?
            frame_scope : rbc_scope_current(s));
        return true;
    }
    if (access.operation == XR_RBC_FUNCTION_CALL) {
        if (!rbc_function_state(s->state[access.source]) || s->origin[access.source] != access.slot) return false;
        XrRbcCallableType type = s->module->callable_types[access.slot - 1];
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            uint32_t r = access.window + 1 + p;
            XrRbcCallableValue value = type.parameters[p].value;
            if (value.kind == XR_RBC_CALLABLE_FUNCTION) {
                if (!rbc_function_state(s->state[r]) || !rbc_callable_accepts(s->module, value.type + 1, s->origin[r])) return false;
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
        if (rbc_scope_current(s)) return false;
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
