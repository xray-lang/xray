/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_admission.inc.c - Owned exact invocation signatures
 */
static bool rbc_callable_value_valid(const XrRbcModule *m, XrRbcCallableValue value, size_t before) {
    switch (value.kind) {
    case XR_RBC_CALLABLE_SCALAR:
        return value.type == XR_POD_UNIT || value.type == XR_POD_BOOL || value.type == XR_POD_I64 ||
            value.type == XR_POD_U8 || value.type == XR_POD_U64 || value.type == XR_POD_F64;
    case XR_RBC_CALLABLE_REFERENCE:
        return value.type == XR_RBC_STRING || value.type == XR_RBC_MAP_STRING_I64 || rbc_array_kind((XrRbcRegisterKind)value.type);
    case XR_RBC_CALLABLE_AGGREGATE: return value.type < m->aggregate_type_count;
    case XR_RBC_CALLABLE_FUNCTION: return value.type < before;
    default: return false;
    }
}

static XrRbcRegisterKind rbc_callable_value_kind(XrRbcCallableValue value) {
    return value.kind == XR_RBC_CALLABLE_REFERENCE ? (XrRbcRegisterKind)value.type :
        value.kind == XR_RBC_CALLABLE_FUNCTION ? XR_RBC_FUNCTION : XR_RBC_BITS;
}

static bool rbc_callable_accepts(const XrRbcModule *m, uint32_t expected, uint32_t actual) {
    if (!m->callable_types || !expected || !actual ||
        expected > m->callable_type_count || actual > m->callable_type_count) return false;
    const XrRbcCallableType *a = &m->callable_types[expected - 1], *b = &m->callable_types[actual - 1];
    if (a->parameter_count != b->parameter_count || a->result.kind != b->result.kind || a->result.type != b->result.type ||
        (a->flags & 2u) != (b->flags & 2u) || ((a->flags & 1u) && !(b->flags & 1u))) return false;
    for (uint32_t p = 0; p < a->parameter_count; ++p) {
        XrRbcCallableParameter x = a->parameters[p], y = b->parameters[p];
        if (x.value.kind != y.value.kind || x.value.type != y.value.type ||
            x.mode != y.mode || x.escaping != y.escaping) return false;
    }
    return true;
}

#include "xrbc_callable_directory.inc.c"

static bool rbc_callable_parameter_owned(const XrRbcModule *m, const XrRbcFunction *f, uint32_t parameter) {
    if (parameter < f->capture_count && f->captures[parameter].kind == XR_RBC_CALLABLE_FUNCTION) return true;
    if (!m->function_callable_parameters) return false;
    XrRbcFunctionCallableParameters view = m->function_callable_parameters[f - m->functions];
    for (uint32_t p = 0; p < view.count; ++p)
        if (view.rows[p].parameter == parameter) return view.rows[p].contract.escaping;
    return false;
}

static bool rbc_callable_metadata_valid(const XrRbcModule *m) {
    if (m->callable_type_count > UINT32_MAX || (!m->callable_types && m->callable_type_count)) return false;
    if (m->callable_results ? m->callable_result_count != m->function_count : m->callable_result_count != 0) return false;
    if (m->function_scopes ? m->function_scope_count != m->function_count : m->function_scope_count != 0) return false;
    if (m->function_callable_parameters ? m->function_callable_parameter_count != m->function_count :
        m->function_callable_parameter_count != 0) return false;
    for (size_t t = 0; t < m->callable_type_count; ++t) {
        XrRbcCallableType type = m->callable_types[t];
        if (type.parameter_count > 65536 || (type.parameter_count && !type.parameters) ||
            (type.flags & ~3u) || ((type.flags & 2u) && !(type.flags & 1u)) ||
            !rbc_callable_value_valid(m, type.result, t)) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableParameter parameter = type.parameters[p];
            if (parameter.mode > 2u || !rbc_callable_value_valid(m, parameter.value, t) ||
                (parameter.escaping && (parameter.value.kind != XR_RBC_CALLABLE_FUNCTION || parameter.mode != 0))) return false;
        }
    }
    for (size_t n = 0; n < m->function_count; ++n) {
        const XrRbcFunction *f = &m->functions[n];
        XrRbcFunctionScopes scopes = rbc_function_scopes(m, f);
        if (scopes.count > f->code_count || (scopes.count ? !scopes.parents : scopes.parents != NULL)) return false;
        for (uint32_t p = 0; p < scopes.count; ++p)
            if (scopes.parents[p] > scopes.count || scopes.parents[p] == p + 1) return false;
        uint32_t result = m->callable_results ? m->callable_results[n] : 0;
        if (f->result_kind == XR_RBC_FUNCTION ? !result || result > m->callable_type_count : result != 0) return false;
        if (f->capture_count > f->parameter_count || f->parameter_count > f->register_count ||
            (f->capture_count ? !f->captures : f->captures != NULL) ||
            (f->environment_count ? !f->environments : f->environments != NULL) ||
            (!m->callable_type_count && (f->environment_count || f->environment_size)) ||
            f->environment_count > UINT16_MAX + 1u) return false;
        if (f->register_callables ? f->register_callable_count != f->register_count :
            f->register_callable_count != 0) return false;
        for (uint32_t p = 0; p < f->capture_count; ++p) {
            XrRbcCallableValue value = f->captures[p];
            if (value.kind == XR_RBC_CALLABLE_CELL) {
                XrXirType type;
                if (!rbc_callable_canonical_type(m, value, &type) ||
                    rbc_register_kind(f, p) != XR_RBC_CAPTURED_CELL_BORROW) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_BINDING) {
                if (value.type != XR_POD_I64 || rbc_register_kind(f, p) != XR_RBC_I64_BINDING) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_FUNCTION) {
                if (!rbc_callable_value_valid(m, value, m->callable_type_count) ||
                    rbc_register_kind(f, p) != XR_RBC_FUNCTION || !f->register_callables ||
                    f->register_callables[p] != value.type + 1) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_REFERENCE) {
                if (value.type != XR_RBC_MAP_STRING_I64 || rbc_register_kind(f, p) != XR_RBC_MAP_STRING_I64) return false;
                continue;
            }
            if (value.kind != XR_RBC_CALLABLE_SCALAR || value.type == XR_POD_UNIT ||
                !rbc_callable_value_valid(m, value, 0) || rbc_register_kind(f, p) != XR_RBC_BITS) return false;
        }
        for (uint32_t r = 0; r < f->register_count; ++r) {
            uint32_t type = f->register_callables ? f->register_callables[r] : 0;
            if (rbc_register_kind(f, r) == XR_RBC_FUNCTION ? !type || type > m->callable_type_count : type != 0)
                return false;
        }
        if (m->function_callable_parameters) {
            XrRbcFunctionCallableParameters parameters = m->function_callable_parameters[n];
            if (parameters.count > f->parameter_count || (parameters.count ? !parameters.rows : parameters.rows != NULL))
                return false;
            for (uint32_t p = 0; p < parameters.count; ++p) {
                XrRbcFunctionCallableParameter row = parameters.rows[p];
                if (row.parameter >= f->parameter_count || (p && parameters.rows[p - 1].parameter >= row.parameter) ||
                    row.contract.mode != 0 || row.contract.value.kind != XR_RBC_CALLABLE_FUNCTION ||
                    !rbc_callable_value_valid(m, row.contract.value, m->callable_type_count) ||
                    rbc_register_kind(f, row.parameter) != XR_RBC_FUNCTION ||
                    f->register_callables[row.parameter] != row.contract.value.type + 1) return false;
            }
        }
        if (!f->callable) {
            if (f->capture_count || f->state_capture) return false;
            continue;
        }
        if (f->callable > m->callable_type_count) return false;
        XrRbcCallableType type = m->callable_types[f->callable - 1];
        if (f->state_capture && (!m->state_layout || f->parameter_count == f->capture_count ||
            rbc_register_kind(f, f->parameter_count - 1) != XR_RBC_STATE)) return false;
        if (type.parameter_count != f->parameter_count - f->capture_count - f->state_capture || f->aggregate_parameter_count ||
            type.result.kind == XR_RBC_CALLABLE_AGGREGATE || type.result.kind == XR_RBC_CALLABLE_FUNCTION ||
            rbc_callable_value_kind(type.result) != f->result_kind) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableValue value = type.parameters[p].value;
            uint32_t formal = f->capture_count + p;
            if (value.kind == XR_RBC_CALLABLE_AGGREGATE || rbc_callable_value_kind(value) != rbc_register_kind(f, formal) ||
                (value.kind == XR_RBC_CALLABLE_FUNCTION && (f->register_callables[formal] != value.type + 1 ||
                 type.parameters[p].escaping != rbc_callable_parameter_owned(m, f, formal)))) return false;
        }
    }
    for (size_t n = 0; m->callable_type_count && n < m->function_count; ++n) {
        const XrRbcFunction *f = &m->functions[n];
        uint32_t bytes = 0;
        for (uint32_t e = 0; e < f->environment_count; ++e) {
            XrRbcEnvironment environment = f->environments[e];
            if (environment.callee >= m->function_count || environment.offset != bytes ||
                !f->aggregate_accesses || environment.producer >= f->aggregate_access_count) return false;
            const XrRbcFunction *callee = &m->functions[environment.callee];
            uint32_t count = callee->capture_count + callee->state_capture;
            if (!count || count > (UINT32_MAX - bytes) / 8u) return false;
            XrRbcAggregateAccess access = f->aggregate_accesses[environment.producer];
            if (access.operation != XR_RBC_FUNCTION_REFERENCE || access.destination != e + 1 ||
                access.callee != environment.callee || access.count != count) return false;
            bytes += count * 8u;
        }
        size_t offset, total;
        if (bytes != f->environment_size || !xr_rbc_activation_size(f, &offset, &total)) return false;
    }
    return rbc_callable_directory_valid(m);
}

static uint32_t rbc_callable_state_type(const XrRbcModule *module, uint32_t type) {
    const XrXirTypeArena *arena = NULL;
    XrXirType signature;
    if (!xrt_module_callable_signature(module->state_layout, type, &arena, &signature)) return 0;
    for (size_t t = 0; t < module->closure_signature_count; ++t)
        if (xir_type_arena_same_type(arena, signature, module->closure_types, module->closure_signatures[t]))
            return (uint32_t)t + 1;
    return 0;
}
static bool rbc_callable_state_valid(const XrRbcModule *m, const XrRbcFunction *f, XrRbcAggregateAccess access) {
    bool read = access.operation == XR_RBC_FUNCTION_STATE_READ;
    bool construct = access.operation == XR_RBC_FUNCTION_STATE_CONSTRUCT;
    bool initialize = access.operation == XR_RBC_FUNCTION_STATE_INITIALIZE;
    if (!m->state_layout || !access.slot || access.slot > m->callable_type_count ||
        access.first || access.count || access.callee || access.result.spill || access.source >= f->register_count)
        return false;
    if (read) return !access.destination && !access.window && access.result.index < f->register_count &&
        rbc_register_kind(f, access.source) == XR_RBC_STATE_REF &&
        rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION &&
        f->register_callables[access.result.index] == access.slot;
    if (access.window >= f->register_count || rbc_register_kind(f, access.window) != XR_RBC_FUNCTION ||
        f->register_callables[access.window] != access.slot) return false;
    if (!construct && !initialize) return !access.destination && !access.result.index &&
        rbc_register_kind(f, access.source) == XR_RBC_STATE_REF;
    XrtModuleLayoutSpec layout = {0};
    if (xrt_module_layout_view(m->state_layout, &layout) || access.destination >= layout.slot_count ||
        rbc_register_kind(f, access.source) != XR_RBC_STATE) return false;
    XrtModuleSlotSpec slot = layout.slots[access.destination];
    return rbc_callable_state_type(m, slot.type) == access.slot &&
        (initialize ? !(slot.flags & XRT_MODULE_SLOT_MUTABLE) && !access.result.index :
         (slot.flags & XRT_MODULE_SLOT_MUTABLE) && access.result.index < f->register_count &&
         rbc_register_kind(f, access.result.index) == XR_RBC_STATE_REF);
}
static bool rbc_callable_access_valid(const XrRbcModule *m, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.operation >= XR_RBC_CAPTURED_CELL_NEW && access.operation <= XR_RBC_CAPTURED_CELL_BIND) {
        if (!m->closure_entry_count || !rbc_captured_cell_type(m, (XrXirType)access.slot) || access.callee)
            return false;
        bool map = rbc_captured_map_type(m, (XrXirType)access.slot);
        bool input = map && (access.operation == XR_RBC_CAPTURED_CELL_NEW || access.operation == XR_RBC_CAPTURED_CELL_WRITE);
        if (input) {
            if (access.count != 1 || access.first >= f->aggregate_operand_count || access.window ||
                !rbc_array_location_valid(f, f->aggregate_operands[access.first]) ||
                rbc_array_location_kind(f, f->aggregate_operands[access.first]) != XR_RBC_MAP_STRING_I64) return false;
        } else if (access.first || access.count) return false;
        if (access.operation == XR_RBC_CAPTURED_CELL_NEW)
            return !access.destination && !access.window && !access.result.spill &&
                (map ? !access.source : access.source < f->register_count && rbc_register_kind(f, access.source) == XR_RBC_BITS) &&
                access.result.index < f->register_count && rbc_register_kind(f, access.result.index) == XR_RBC_CAPTURED_CELL;
        if (access.source >= f->register_count || (rbc_register_kind(f, access.source) != XR_RBC_CAPTURED_CELL &&
                rbc_register_kind(f, access.source) != XR_RBC_CAPTURED_CELL_BORROW)) return false;
        if (access.operation == XR_RBC_CAPTURED_CELL_BIND) {
            XrRbcFunctionScopes scopes = rbc_function_scopes(m, f);
            return access.destination && access.destination <= scopes.count && access.window != UINT32_MAX &&
                !access.result.spill && access.result.index < f->register_count &&
                rbc_register_kind(f, access.result.index) == XR_RBC_CAPTURED_CELL;
        }
        if (access.destination) return false;
        if (access.operation == XR_RBC_CAPTURED_CELL_READ)
            return !access.window && rbc_array_location_valid(f, access.result) &&
                rbc_array_location_kind(f, access.result) == (map ? XR_RBC_MAP_STRING_I64 : XR_RBC_BITS);
        return !access.result.index && !access.result.spill && (map ||
            (access.window < f->register_count && rbc_register_kind(f, access.window) == XR_RBC_BITS));
    }
    if (access.operation >= XR_RBC_FUNCTION_STATE_READ && access.operation <= XR_RBC_FUNCTION_STATE_INITIALIZE)
        return rbc_callable_state_valid(m, f, access);
    if (access.operation >= XR_RBC_FUNCTION_SCOPE_ENTER && access.operation <= XR_RBC_FUNCTION_BIND) {
        XrRbcFunctionScopes scopes = rbc_function_scopes(m, f);
        if (!access.destination || access.destination > scopes.count || access.first || access.callee || access.result.spill)
            return false;
        if (access.operation == XR_RBC_FUNCTION_SCOPE_ENTER)
            return access.source == scopes.parents[access.destination - 1] && !access.result.index &&
                !access.count && !access.slot && !access.window;
        if (access.operation == XR_RBC_FUNCTION_BIND ? access.count != 1 || access.window == UINT32_MAX :
                access.count > 1 || access.window != 0) return false;
        if (!access.count) return !access.source && !access.result.index && !access.slot;
        return access.source < f->register_count && access.result.index < f->register_count &&
            rbc_register_kind(f, access.source) == XR_RBC_FUNCTION &&
            rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION && access.slot &&
            f->register_callables[access.source] == access.slot && f->register_callables[access.result.index] == access.slot;
    }
    if (access.operation == XR_RBC_FUNCTION_SCOPE_RETURN) {
        uint32_t result = m->callable_results ? m->callable_results[f - m->functions] : 0;
        if (access.slot != result || access.first || access.destination || access.callee || access.window ||
            access.result.index || access.result.spill || access.count > 1) return false;
        if (f->callable) {
            XrRbcCallableValue declared = m->callable_types[f->callable - 1].result;
            if (declared.kind == XR_RBC_CALLABLE_SCALAR && declared.type == XR_POD_UNIT)
                return !access.count && !access.source;
            if (access.count != 1) return false;
        }
        if (!access.count) return !access.source && f->result_kind == XR_RBC_BITS;
        return access.source < f->register_count && rbc_register_kind(f, access.source) == f->result_kind;
    }
    if (access.operation == XR_RBC_FUNCTION_RETURN) {
        if (!f->callable || access.slot != f->callable || access.first || access.destination ||
            access.callee || access.window || access.result.index || access.result.spill) return false;
        XrRbcCallableValue result = m->callable_types[f->callable - 1].result;
        if (result.kind == XR_RBC_CALLABLE_SCALAR && result.type == XR_POD_UNIT)
            return !access.count && !access.source;
        return access.count == 1 && access.source < f->register_count &&
            rbc_register_kind(f, access.source) == rbc_callable_value_kind(result);
    }
    if (!access.slot || access.slot > m->callable_type_count || access.result.spill) return false;
    if (access.operation == XR_RBC_FUNCTION_REFERENCE || access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) {
        if (access.source || access.window || access.callee >= m->function_count) return false;
        const XrRbcFunction *callee = &m->functions[access.callee];
        uint32_t count = callee->capture_count + callee->state_capture;
        if (access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) {
            if (callee->state_capture || !access.destination ||
                access.destination > m->closure_entry_count ||
                m->closure_targets[access.destination - 1] != access.callee || access.count != count ||
                access.first > f->aggregate_operand_count || count > f->aggregate_operand_count - access.first) return false;
            for (uint32_t p = 0; p < count; ++p)
                if ((callee->captures[p].kind != XR_RBC_CALLABLE_SCALAR && callee->captures[p].kind != XR_RBC_CALLABLE_CELL &&
                     callee->captures[p].kind != XR_RBC_CALLABLE_FUNCTION &&
                     callee->captures[p].kind != XR_RBC_CALLABLE_REFERENCE) ||
                    !rbc_array_location_valid(f, f->aggregate_operands[access.first + p]) ||
                    (callee->captures[p].kind == XR_RBC_CALLABLE_CELL ?
                        (rbc_array_location_kind(f, f->aggregate_operands[access.first + p]) != XR_RBC_CAPTURED_CELL &&
                         rbc_array_location_kind(f, f->aggregate_operands[access.first + p]) != XR_RBC_CAPTURED_CELL_BORROW) :
                        rbc_array_location_kind(f, f->aggregate_operands[access.first + p]) !=
                            rbc_callable_value_kind(callee->captures[p]))) return false;
        } else if (count) {
            if (!access.destination || access.destination > f->environment_count || access.count != count ||
                access.first > f->aggregate_operand_count || count > f->aggregate_operand_count - access.first ||
                f->environments[access.destination - 1].callee != access.callee) return false;
            for (uint32_t p = 0; p < count; ++p) {
                XrRbcArrayLocation location = f->aggregate_operands[access.first + p];
                if (p < callee->capture_count && callee->captures[p].kind == XR_RBC_CALLABLE_FUNCTION) return false;
                if (p < callee->capture_count && callee->captures[p].kind == XR_RBC_CALLABLE_CELL) {
                    if (!rbc_array_location_valid(f, location) ||
                        (rbc_array_location_kind(f, location) != XR_RBC_CAPTURED_CELL &&
                         rbc_array_location_kind(f, location) != XR_RBC_CAPTURED_CELL_BORROW)) return false;
                    continue;
                }
                XrRbcRegisterKind kind = p < callee->capture_count ?
                    (callee->captures[p].kind == XR_RBC_CALLABLE_BINDING ? XR_RBC_I64_BINDING :
                     XR_RBC_BITS) : XR_RBC_STATE;
                if (!rbc_array_location_valid(f, location) || rbc_array_location_kind(f, location) != kind) return false;
            }
        } else if (access.first || access.count || access.destination) return false;
        return
            rbc_callable_accepts(m, access.slot, m->functions[access.callee].callable) &&
            access.result.index < f->register_count && rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION &&
            f->register_callables[access.result.index] == access.slot;
    }
    if (access.first || access.count || access.destination) return false;
    XrRbcCallableType type = m->callable_types[access.slot - 1];
    if (access.callee || access.result.index || access.source >= f->register_count ||
        rbc_register_kind(f, access.source) != XR_RBC_FUNCTION ||
        f->register_callables[access.source] != access.slot || access.window >= f->register_count ||
        type.parameter_count != f->register_count - access.window - 1 ||
        type.result.kind == XR_RBC_CALLABLE_FUNCTION || type.result.kind == XR_RBC_CALLABLE_AGGREGATE) return false;
    for (uint32_t p = 0; p <= type.parameter_count; ++p)
        if (rbc_register_kind(f, access.window + p) != XR_RBC_CALL_WINDOW) return false;
    return true;
}
