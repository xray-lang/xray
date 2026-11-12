/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_verify.inc.c - Exact environment and indirect operand matching
 */
static bool lir_callable_result_matches(const XrLirCallableType *a, const XrLirCallableType *b) {
    if (a->parameter_count != b->parameter_count || a->result != b->result ||
        a->result_element != b->result_element || a->throws != b->throws ||
        (a->flags & XR_LIR_CALLABLE_C_ABI) != (b->flags & XR_LIR_CALLABLE_C_ABI) ||
        ((a->flags & XR_LIR_CALLABLE_NO_SUSPEND) && !(b->flags & XR_LIR_CALLABLE_NO_SUSPEND)))
        return false;
    for (uint32_t p = 0; p < a->parameter_count; ++p) {
        XrLirCallableParameter expected = a->parameters[p], actual = b->parameters[p];
        if (expected.type != actual.type || expected.array_element != actual.array_element ||
            expected.mode != actual.mode || expected.escaping != actual.escaping) return false;
    }
    return true;
}

XR_FUNC bool xr_lir_internal_callable_instruction(const XrLirModule *m,
    const XrLirFunctionData *f, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_FUNCTION_REF) {
        XrLirCallableType signature, target_signature;
        if (v->bits || !v->callee || v->callee > m->function_count ||
            xr_lir_callable_type_view(m, v->type, &signature) != XR_LIR_OK) return false;
        const XrLirFunctionData *target = &m->functions[v->callee - 1];
        XrLirClosureTarget closure = target->closure;
        if (!closure.callable || !xr_lir_internal_closure_target_valid(m, target) ||
            xr_lir_callable_type_view(m, closure.callable, &target_signature) != XR_LIR_OK ||
            !lir_callable_result_matches(&signature, &target_signature) ||
            v->operand_count != closure.capture_count + closure.state_capture) return false;
        if ((signature.flags & XR_LIR_CALLABLE_C_ABI) && v->operand_count) return false;
        for (uint32_t p = 0; p < closure.capture_count; ++p) {
            const XrLirValueData *actual = &f->values[v->operands[p] - 1];
            if (actual->type != target->parameter_types[p] ||
                (xr_lir_internal_array_type(actual->type) &&
                    actual->array_element != target->values[p].array_element)) return false;
        }
        return !closure.state_capture ||
            f->values[v->operands[closure.capture_count] - 1].type == XR_LIR_STATE;
    }
    if (v->op == XR_LIR_CALL_INDIRECT) {
        XrLirCallableType signature;
        if (!v->operand_count || xr_lir_callable_type_view(m,
            f->values[v->operands[0] - 1].type, &signature) != XR_LIR_OK ||
            v->operand_count - 1 != signature.parameter_count || v->type != signature.result)
            return false;
        for (uint32_t p = 0; p < signature.parameter_count; ++p) {
            const XrLirValueData *argument = &f->values[v->operands[p + 1] - 1];
            if (argument->type != signature.parameters[p].type ||
                (xr_lir_internal_array_type(argument->type) &&
                    argument->array_element != signature.parameters[p].array_element)) return false;
        }
    }
    return true;
}
