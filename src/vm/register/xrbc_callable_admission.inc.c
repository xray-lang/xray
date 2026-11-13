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
        return value.type == XR_RBC_STRING || rbc_array_kind((XrRbcRegisterKind)value.type);
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

static bool rbc_callable_metadata_valid(const XrRbcModule *m) {
    if (m->callable_type_count > UINT32_MAX || (!m->callable_types && m->callable_type_count)) return false;
    for (size_t t = 0; t < m->callable_type_count; ++t) {
        XrRbcCallableType type = m->callable_types[t];
        if (type.parameter_count > 65536 || (type.parameter_count && !type.parameters) ||
            (type.flags & ~3u) || ((type.flags & 2u) && !(type.flags & 1u)) ||
            !rbc_callable_value_valid(m, type.result, t)) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableParameter parameter = type.parameters[p];
            if (parameter.mode > 2u || !rbc_callable_value_valid(m, parameter.value, t) ||
                (parameter.escaping && parameter.value.kind != XR_RBC_CALLABLE_FUNCTION)) return false;
        }
    }
    for (size_t n = 0; n < m->function_count; ++n) {
        const XrRbcFunction *f = &m->functions[n];
        if (f->register_callables ? f->register_callable_count != f->register_count :
            f->register_callable_count != 0) return false;
        for (uint32_t r = 0; r < f->register_count; ++r) {
            uint32_t type = f->register_callables ? f->register_callables[r] : 0;
            if (rbc_register_kind(f, r) == XR_RBC_FUNCTION ? !type || type > m->callable_type_count : type != 0)
                return false;
        }
        if (!f->callable) continue;
        if (f->callable > m->callable_type_count) return false;
        XrRbcCallableType type = m->callable_types[f->callable - 1];
        if (type.parameter_count != f->parameter_count || f->aggregate_parameter_count ||
            type.result.kind == XR_RBC_CALLABLE_AGGREGATE || type.result.kind == XR_RBC_CALLABLE_FUNCTION ||
            rbc_callable_value_kind(type.result) != f->result_kind) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableValue value = type.parameters[p].value;
            if (value.kind == XR_RBC_CALLABLE_AGGREGATE || rbc_callable_value_kind(value) != rbc_register_kind(f, p) ||
                (value.kind == XR_RBC_CALLABLE_FUNCTION && f->register_callables[p] != value.type + 1)) return false;
        }
    }
    return true;
}

static bool rbc_callable_access_valid(const XrRbcModule *m, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.operation == XR_RBC_FUNCTION_RETURN) {
        if (!f->callable || access.slot != f->callable || access.first || access.destination ||
            access.callee || access.window || access.result.index || access.result.spill) return false;
        XrRbcCallableValue result = m->callable_types[f->callable - 1].result;
        if (result.kind == XR_RBC_CALLABLE_SCALAR && result.type == XR_POD_UNIT)
            return !access.count && !access.source;
        return access.count == 1 && access.source < f->register_count &&
            rbc_register_kind(f, access.source) == rbc_callable_value_kind(result);
    }
    if (!access.slot || access.slot > m->callable_type_count || access.first || access.count ||
        access.destination || access.result.spill) return false;
    if (access.operation == XR_RBC_FUNCTION_REFERENCE)
        return !access.source && !access.window && access.callee < m->function_count &&
            rbc_callable_accepts(m, access.slot, m->functions[access.callee].callable) &&
            access.result.index < f->register_count && rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION &&
            f->register_callables[access.result.index] == access.slot;
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
