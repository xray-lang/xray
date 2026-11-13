/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_callable.inc.c - Exact ordinary C invocation signatures
 *
 * KEY CONCEPT:
 *   Typed thunks keep function pointers callable without casts. The separate
 *   environment and owner fields cannot create a code address from raw data.
 */
static void c_callable_type(LirC *c, XrLirType type) {
    XrLirCallableType signature;
    if (xr_lir_callable_type_view(c->request->module, type, &signature) != XR_LIR_OK) {
        c_error(c, XR_LIR_C_INVALID_LIR, "callable type has no owned signature");
        return;
    }
    const char *name = c_type(c, type);
    c_format(c, "typedef struct %s { %s (*invoke)(void *", name, c_type(c, signature.result));
    for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p)
        c_format(c, ", %s", c_type(c, signature.parameters[p].type));
    c_format(c, "); void *environment; void *owner; } %s;\n", name);
}

static void c_callable_thunks(LirC *c) {
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        XrLirFunction function = {c->request->module, c->closure[n]};
        if (!(c->seen[function.id - 1] & 2u)) continue;
        XrLirClosureTarget target = {0};
        if (xr_lir_closure_target_view(c->request->module, function, &target) != XR_LIR_OK ||
            !target.callable || target.capture_count || target.state_capture) continue;
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(c->request->module, target.callable, &signature) != XR_LIR_OK) {
            c_error(c, XR_LIR_C_INVALID_LIR, "closure code has no exact invocation signature");
            return;
        }
        c_format(c, "static %s xrl_thunk_%" PRIu32 "(void *environment",
            c_type(c, signature.result), function.id);
        for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p)
            c_format(c, ", %s a%" PRIu32, c_type(c, signature.parameters[p].type), p);
        c_text(c, ") {\n    (void)environment;\n    ");
        if (signature.result != XR_LIR_UNIT) c_text(c, "return ");
        c_function_name(c, function.id);
        c_text(c, "(");
        for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p)
            c_format(c, p ? ", a%" PRIu32 : "a%" PRIu32, p);
        c_text(c, ");\n}\n");
    }
}

static void c_callable_argument(LirC *c, uint32_t value, XrLirType expected) {
    if (c_value_type(c, value) == expected) { c_value(c, value); return; }
    c_format(c, "(%s){", c_type(c, expected));
    c_value(c, value); c_text(c, ".invoke, ");
    c_value(c, value); c_text(c, ".environment, ");
    c_value(c, value); c_text(c, ".owner}");
}

static void c_callable_instruction(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_FUNCTION_REF) {
        c_format(c, "    v%" PRIu32 " = (%s){xrl_thunk_%" PRIu32 ", NULL, NULL};\n",
            v->result, c_type(c, v->type), v->callee);
        return;
    }
    c_text(c, "    ");
    if (v->result) c_format(c, "v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, ".invoke(");
    c_value(c, v->operands[0]);
    c_text(c, ".environment");
    XrLirCallableType signature;
    xr_lir_callable_type_view(c->request->module, c_value_type(c, v->operands[0]), &signature);
    for (uint32_t p = 1; p < v->operand_count && c_request_poll(c); ++p) {
        c_text(c, ", ");
        c_callable_argument(c, v->operands[p], signature.parameters[p - 1].type);
    }
    c_text(c, ");\n");
}
