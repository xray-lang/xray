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

static bool c_callable_frame_target(LirC *c, const XrLirInstructionView *instruction) {
    XrLirClosureTarget target = {0};
    XrLirFunctionView function = {0};
    XrLirFunction callee = {c->request->module, instruction->callee};
    if (instruction->bits > XR_LIR_CLOSURE_OWNED ||
        xr_lir_closure_target_view(c->request->module, callee, &target) != XR_LIR_OK ||
        xr_lir_function_view(c->request->module, callee, &function) != XR_LIR_OK) return false;
    if (instruction->bits == XR_LIR_CLOSURE_OWNED && target.state_capture) return false;
    if (instruction->operand_count) {
        if (c->block != c->view.entry) return false;
        if (!(c->seen[c->function.id - 1] & 4u)) {
            for (uint32_t b = 0; b < c->view.block_count; ++b) {
                XrLirBlock block;
                XrLirBlockView view;
                XrLirInstructionView end;
                xr_lir_block_at(c->request->module, c->function, b, &block);
                xr_lir_block_view(c->request->module, block, &view);
                xr_lir_instruction_view(c->request->module, block, view.instruction_count - 1, &end);
                for (uint32_t e = 0; e < end.edge_count; ++e)
                    if (end.edges[e].target == c->view.entry) return false;
            }
            c->seen[c->function.id - 1] |= 4u;
        }
    }
    for (uint32_t p = 0; p < target.capture_count; ++p) {
        XrLirType type = function.parameter_types[p];
        XrLirCellType cell;
        if (xr_lir_cell_type_view(c->request->module, type, &cell) == XR_LIR_OK && cell.payload == XR_LIR_I64)
            continue;
        if (type != XR_LIR_I1 && type != XR_LIR_I64 && type != XR_LIR_U8 &&
            type != XR_LIR_U64 && type != XR_LIR_F64 &&
            (instruction->bits == XR_LIR_CLOSURE_OWNED || type != XR_LIR_I64_CELL)) return false;
    }
    return true;
}

static bool c_callable_owned_return(const LirC *c, uint32_t value) {
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    for (uint32_t steps = 0; steps < function->value_count; ++steps) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(function, value);
        if (!definition) return false;
        if (definition->op == XR_LIR_FUNCTION_REF)
            return definition->bits == XR_LIR_CLOSURE_OWNED || !definition->operand_count;
        if (definition->op == XR_LIR_CALL || definition->op == XR_LIR_CALL_INDIRECT ||
            definition->op == XR_LIR_STATE_READ) return true;
        if ((definition->op != XR_LIR_FUNCTION_BIND && definition->op != XR_LIR_FUNCTION_SCOPE_LEAVE) ||
            definition->operand_count != 2) return false;
        value = definition->operands[1];
    }
    return false;
}

#include "xlir_c_callable_owned.inc.c"

static void c_callable_environment(LirC *c, uint32_t function, const XrLirFunctionView *view,
    XrLirClosureTarget target) {
    c_format(c, "typedef struct XrlEnv_%" PRIu32 " {", function);
    for (uint32_t p = 0; p < target.capture_count; ++p)
        c_format(c, " %s c%" PRIu32 ";", c_type(c, view->parameter_types[p]), p);
    if (target.state_capture) c_text(c, " XrtModuleState *state;");
    c_format(c, " } XrlEnv_%" PRIu32 ";\n", function);
}

static void c_callable_thunks(LirC *c) {
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        XrLirFunction function = {c->request->module, c->closure[n]};
        if (!(c->seen[function.id - 1] & 2u)) continue;
        XrLirClosureTarget target = {0};
        if (xr_lir_closure_target_view(c->request->module, function, &target) != XR_LIR_OK || !target.callable) continue;
        XrLirFunctionView view = {0};
        xr_lir_function_view(c->request->module, function, &view);
        bool environment = target.capture_count || target.state_capture;
        if (environment) c_callable_environment(c, function.id, &view, target);
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(c->request->module, target.callable, &signature) != XR_LIR_OK) {
            c_error(c, XR_LIR_C_INVALID_LIR, "closure code has no exact invocation signature");
            return;
        }
        c_format(c, "static %s xrl_thunk_%" PRIu32 "(void *environment",
            c_type(c, signature.result), function.id);
        for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p)
            c_format(c, ", %s a%" PRIu32, c_type(c, signature.parameters[p].type), p);
        c_text(c, ") {\n    ");
        if (environment) c_format(c, "XrlEnv_%" PRIu32 " *bound = environment;\n    ", function.id);
        else c_text(c, "(void)environment;\n    ");
        if (signature.result != XR_LIR_UNIT) c_text(c, "return ");
        c_function_name(c, function.id);
        c_text(c, "(");
        for (uint32_t p = 0; p < target.capture_count; ++p)
            c_format(c, p ? ", bound->c%" PRIu32 : "bound->c%" PRIu32, p);
        for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p)
            c_format(c, p || target.capture_count ? ", a%" PRIu32 : "a%" PRIu32, p);
        if (target.state_capture) c_text(c, target.capture_count || signature.parameter_count ? ", bound->state" : "bound->state");
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
        if (v->bits == XR_LIR_CLOSURE_OWNED) {
            c_callable_owned_reference(c, v);
            return;
        }
        if (v->operand_count) {
            c_format(c, "    xrl_env%" PRIu32 " = (XrlEnv_%" PRIu32 "){", v->result, v->callee);
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                if (p) c_text(c, ", ");
                c_value(c, v->operands[p]);
            }
            c_text(c, "};\n");
            c_format(c, "    v%" PRIu32 " = (%s){xrl_thunk_%" PRIu32 ", &xrl_env%" PRIu32 ", NULL};\n",
                v->result, c_type(c, v->type), v->callee, v->result);
        } else c_format(c, "    v%" PRIu32 " = (%s){xrl_thunk_%" PRIu32 ", NULL, NULL};\n",
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
