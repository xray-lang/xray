/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_types.inc.c - Structural identity for callable signatures
 *
 * KEY CONCEPT:
 *   Hashing and equality read initialized fields, never padding or addresses.
 *   Every dependency precedes its receiving type in the acyclic owned pool.
 */
static bool lir_callable_array_shape(XrLirType type, XrLirType element) {
    return xr_lir_internal_array_type(type) ? xr_lir_internal_array_element(element) : !element;
}

static bool lir_callable_signature_valid(const XrLirModule *m, const XrLirCallableType *type,
    uint32_t ordinal) {
    if (!type || type->throws || type->parameter_count > 65536 ||
        (type->parameter_count && !type->parameters) ||
        (type->flags & ~(XR_LIR_CALLABLE_NO_SUSPEND | XR_LIR_CALLABLE_C_ABI)) ||
        !xr_lir_internal_type_valid(m, type->result, true) ||
        type->result >= XR_LIR_TYPE_COUNT + ordinal ||
        !lir_callable_array_shape(type->result, type->result_element)) return false;
    if ((type->flags & XR_LIR_CALLABLE_C_ABI) &&
        (!(type->flags & XR_LIR_CALLABLE_NO_SUSPEND) || type->throws)) return false;
    for (uint32_t p = 0; p < type->parameter_count; ++p) {
        XrLirCallableParameter parameter = type->parameters[p];
        if (!xr_lir_internal_type_valid(m, parameter.type, false) ||
            parameter.type >= XR_LIR_TYPE_COUNT + ordinal ||
            !xr_param_mode_is_valid(parameter.mode) ||
            !lir_callable_array_shape(parameter.type, parameter.array_element)) return false;
        if (parameter.escaping && (parameter.type < XR_LIR_TYPE_COUNT ||
            m->types[parameter.type - XR_LIR_TYPE_COUNT].shape.kind != XR_LIR_TYPE_CALLABLE)) return false;
    }
    return true;
}

static uint32_t lir_callable_hash(uint32_t hash, const XrLirCallableType *type) {
    uint32_t fields[] = {type->flags, type->throws, type->result_element};
    hash ^= xr_hash_bytes(fields, sizeof(fields));
    for (uint32_t p = 0; p < type->parameter_count; ++p) {
        XrLirCallableParameter parameter = type->parameters[p];
        uint32_t values[] = {hash, parameter.type, parameter.array_element,
            (uint32_t)parameter.mode, parameter.escaping};
        hash = xr_hash_bytes(values, sizeof(values));
    }
    return hash;
}

static bool lir_callable_equal(const XrLirCallableType *a, const XrLirCallableType *b) {
    if (a->flags != b->flags || a->throws != b->throws || a->result != b->result ||
        a->result_element != b->result_element || a->parameter_count != b->parameter_count)
        return false;
    for (uint32_t p = 0; p < a->parameter_count; ++p) {
        XrLirCallableParameter left = a->parameters[p], right = b->parameters[p];
        if (left.type != right.type || left.array_element != right.array_element ||
            left.mode != right.mode || left.escaping != right.escaping) return false;
    }
    return true;
}

XR_FUNC XrLirStatus xr_lir_callable_type_view(const XrLirModule *m, XrLirType type,
    XrLirCallableType *output) {
    if (!m || !output || m->failure != XR_LIR_OK || type < XR_LIR_TYPE_COUNT ||
        type - XR_LIR_TYPE_COUNT >= m->type_count ||
        m->types[type - XR_LIR_TYPE_COUNT].shape.kind != XR_LIR_TYPE_CALLABLE)
        return XR_LIR_INVALID_ARGUMENT;
    *output = m->types[type - XR_LIR_TYPE_COUNT].callable;
    return XR_LIR_OK;
}
