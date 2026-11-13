/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_verify.inc.c - Exact element admission through real SSA producers
 */
static bool lir_array_function_shape(const XrLirFunctionData *f) {
    if (f->result == XR_LIR_ARRAY_CELL || f->result == XR_LIR_ARRAY_REF || (f->result == XR_LIR_ARRAY ?
        !xr_lir_internal_array_element(f->array_result) : f->array_result != XR_LIR_TYPE_INVALID))
        return false;
    for (uint32_t v = 0; v < f->value_count; ++v)
        if (xr_lir_internal_array_type(f->values[v].type) ?
            !xr_lir_internal_array_element(f->values[v].array_element) :
            f->values[v].array_element != XR_LIR_TYPE_INVALID) return false;
    if (f->kind == XR_LIR_FUNCTION_EXTERNAL) {
        if (f->result == XR_LIR_ARRAY) return false;
        for (uint32_t p = 0; p < f->parameter_count; ++p)
            if (xr_lir_internal_array_type(f->parameter_types[p])) return false;
    }
    return true;
}

static bool lir_array_call_shape(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    const XrLirFunctionData *callee = &m->functions[v->callee - 1];
    for (uint32_t p = 0; p < v->operand_count; ++p) {
        const XrLirValueData *actual = &f->values[v->operands[p] - 1];
        if (xr_lir_internal_array_type(actual->type) &&
            (!xr_lir_internal_array_element(actual->array_element) ||
             actual->array_element != callee->values[callee->parameters[p] - 1].array_element)) return false;
    }
    return true;
}

static bool lir_array_intrinsic_call(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    const XrLirArrayIntrinsic *shape = xr_lir_internal_array_intrinsic(m, v->intrinsic);
    if (!shape) {
        if (xr_lir_internal_array_type(v->type)) return false;
        for (uint32_t p = 0; p < v->operand_count; ++p)
            if (xr_lir_internal_array_type(f->values[v->operands[p] - 1].type)) return false;
        return true;
    }
    for (uint32_t p = 0; p < v->operand_count; ++p) {
        const XrLirValueData *actual = &f->values[v->operands[p] - 1];
        if (xr_lir_internal_array_type(actual->type) &&
            (p >= shape->parameter_count || actual->array_element != shape->parameter_elements[p])) return false;
    }
    return true;
}

static bool lir_array_intrinsic_signature(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v, const XrLirIntrinsic *entry, const XrLirArrayIntrinsic *shape) {
    if (!entry->scalar_sync || !xr_lir_internal_array_intrinsic_valid(entry, shape)) return false;
    if (!shape->variadic)
        return lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count, entry->result);
    if (v->type != entry->result) return false;
    for (uint32_t p = 0; p < v->operand_count; ++p)
        if (f->values[v->operands[p] - 1].type != entry->parameters[0]) return false;
    return true;
}

XR_FUNC bool xr_lir_internal_array_instruction(const XrLirModule *m,
    const XrLirFunctionData *f, const XrLirInstructionView *v) {
    if (xr_lir_internal_array_type(v->type) &&
        !xr_lir_internal_array_element(xr_lir_internal_array_result(m, f, v))) return false;
    if (v->result && v->result <= f->value_count &&
        f->values[v->result - 1].array_element != xr_lir_internal_array_result(m, f, v)) return false;
    if (v->op == XR_LIR_CALL) return lir_array_call_shape(m, f, v);
    if (v->op == XR_LIR_CALL_INTRINSIC) return lir_array_intrinsic_call(m, f, v);
    if (v->op == XR_LIR_RET && f->result == XR_LIR_ARRAY)
        return v->operand_count == 1 &&
            f->values[v->operands[0] - 1].array_element == f->array_result;
    if (v->op == XR_LIR_CELL_WRITE && f->values[v->operands[0] - 1].type == XR_LIR_ARRAY_CELL)
        return f->values[v->operands[0] - 1].array_element == f->values[v->operands[1] - 1].array_element;
    if (v->op == XR_LIR_ARRAY_REF_WRITE)
        return f->values[v->operands[0] - 1].array_element == f->values[v->operands[1] - 1].array_element;
    return true;
}
