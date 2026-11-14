/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_callable_owned.inc.c - Canonical owners behind exact C invocation
 */
static bool c_callable_scope_point(const XrLirInstructionView *instruction) {
    return instruction->op == XR_LIR_FUNCTION_SCOPE_ENTER || instruction->op == XR_LIR_FUNCTION_SCOPE_LEAVE ||
        instruction->op == XR_LIR_FUNCTION_BIND;
}
static bool c_callable_owned_point(const XrLirInstructionView *instruction) {
    return instruction->op == XR_LIR_FUNCTION_REF && instruction->bits == XR_LIR_CLOSURE_OWNED;
}
static void c_callable_scope_instruction(LirC *c, const XrLirInstructionView *v) {
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER) {
        c_format(c, "    v%" PRIu32 " = &xrl_scope_storage_%" PRIu32 ";\n", v->result, v->result);
        c_format(c, "    xrt_callable_scope_enter(v%" PRIu32 ", &xrl_callable_image, %" PRIu32 ");\n"
            "    xrl_scope = v%" PRIu32 ";\n", v->result, source, v->result);
        return;
    }
    uint32_t scope = v->operands[0];
    if (v->result) {
        c_format(c, "    v%" PRIu32 " = ", v->result);
        c_value(c, v->operands[1]);
        c_text(c, ";\n");
    }
    if (v->op == XR_LIR_FUNCTION_BIND) {
        XrLirCellType cell;
        if (xr_lir_cell_type_view(c->request->module, v->type, &cell) == XR_LIR_OK) {
            c_format(c, "    v%" PRIu32 " = xrt_callable_cell_scope_bind(", v->result);
            c_value(c, scope);
            c_format(c, ", UINT32_C(%" PRIu32 "), v%" PRIu32 ", %" PRIu32 ");\n",
                (uint32_t)v->bits, v->result, source);
            return;
        }
        c_format(c, "    v%" PRIu32 ".owner = xrt_callable_scope_bind(", v->result);
        c_value(c, scope);
        c_format(c, ", UINT32_C(%" PRIu32 "), v%" PRIu32 ".owner, %" PRIu32 ");\n",
            (uint32_t)v->bits, v->result, source);
        return;
    }
    if (v->result) c_format(c, "    v%" PRIu32 ".owner = ", v->result);
    else c_text(c, "    (void)");
    c_text(c, "xrt_callable_scope_leave(");
    c_value(c, scope);
    if (v->result) c_format(c, ", v%" PRIu32 ".owner, %" PRIu32 ");\n", v->result, source);
    else c_format(c, ", NULL, %" PRIu32 ");\n", source);
    c_text(c, "    xrl_scope = ");
    c_value(c, scope);
    c_text(c, "->parent;\n");
}
static void c_callable_owned_target(LirC *c, uint32_t target) {
    if (!c->owned_entries) {
        void *storage = NULL;
        if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
                xr_lir_function_count(c->request->module), sizeof(*c->owned_entries), &storage))) return;
        c->owned_entries = storage;
    }
    if (!c->owned_entries[target - 1]) c->owned_entries[target - 1] = ++c->owned_entry_count;
}
static void c_callable_owned_catalog(LirC *c) {
    if (!c->owned_entry_count) return;
    uint32_t count = xr_lir_constructed_type_count(c->request->module), capacity = 0;
    for (uint32_t t = 0; t < count; ++t) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(c->request->module, XR_LIR_TYPE_COUNT + t, &signature) == XR_LIR_OK &&
            signature.parameter_count > capacity) capacity = signature.parameter_count;
    }
    XrtCallableParameterSpec *parameters = NULL;
    void *storage = NULL;
    if (capacity && !c_resource(c, xr_compile_resources_calloc(c->request->resources,
            capacity, sizeof(*parameters), &storage))) return;
    parameters = storage;
    for (uint32_t t = 0; t < count && c_request_poll(c); ++t) {
        XrtCallableTypeSpec spec;
        if (lir_native_callable_spec(c->request->module, XR_LIR_TYPE_COUNT + t, parameters, capacity, &spec) != XR_LIR_OK) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "canonical closure type directory cannot receive this physical shape");
            xr_compile_resources_free(parameters);
            return;
        }
        if (!spec.parameter_count) continue;
        c_format(c, "static const XrtCallableParameterSpec xrl_callable_parameters_%" PRIu32 "[] = {\n", t);
        for (uint32_t p = 0; p < spec.parameter_count; ++p)
            c_format(c, "    {{%u, %" PRIu32 "}, %" PRIu32 ", %u},\n",
                (unsigned)parameters[p].value.kind, parameters[p].value.type,
                parameters[p].mode, (unsigned)parameters[p].escaping);
        c_text(c, "};\n");
    }
    c_text(c, "static const XrtCallableTypeSpec xrl_callable_types[] = {\n");
    for (uint32_t t = 0; t < count && c_request_poll(c); ++t) {
        XrtCallableTypeSpec spec;
        if (lir_native_callable_spec(c->request->module, XR_LIR_TYPE_COUNT + t, parameters, capacity, &spec) != XR_LIR_OK) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "canonical closure type directory changed during emission");
            xr_compile_resources_free(parameters);
            return;
        }
        c_format(c, "    {%u, {%u, %" PRIu32 "}, %" PRIu32 ", ",
            (unsigned)spec.kind, (unsigned)spec.element.kind, spec.element.type, spec.count);
        if (spec.parameter_count) c_format(c, "xrl_callable_parameters_%" PRIu32, t);
        else c_text(c, "NULL");
        c_format(c, ", %" PRIu32 ", {%u, %" PRIu32 "}, %" PRIu32 ", 0},\n",
            spec.parameter_count, (unsigned)spec.result.kind, spec.result.type, spec.flags);
    }
    c_text(c, "};\n");
    uint32_t next = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
        if (!c->owned_entries[f]) continue;
        c->owned_entries[f] = ++next;
        XrLirClosureTarget target;
        XrLirFunctionView view;
        XrLirFunction function = {c->request->module, f + 1};
        xr_lir_closure_target_view(c->request->module, function, &target);
        xr_lir_function_view(c->request->module, function, &view);
        if (!target.capture_count) continue;
        c_format(c, "static const XrtCallableValueSpec xrl_callable_captures_%" PRIu32 "[] = {", f + 1);
        for (uint32_t p = 0; p < target.capture_count; ++p) {
            XrtCallableValueSpec value;
            if (!lir_native_callable_value(view.parameter_types[p], 0, &value)) {
                c_error(c, XR_LIR_C_UNSUPPORTED, "capture has no canonical physical type"); break;
            }
            c_format(c, "{%u, %" PRIu32 "},", (unsigned)value.kind, value.type);
        }
        c_text(c, "};\n");
    }
    c_text(c, "static const XrtCallableEntrySpec xrl_callable_entries[] = {\n");
    for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
        if (!c->owned_entries[f]) continue;
        XrLirClosureTarget target;
        xr_lir_closure_target_view(c->request->module, (XrLirFunction){c->request->module, f + 1}, &target);
        c_format(c, "    {%" PRIu32 ", ", target.callable - XR_LIR_TYPE_COUNT);
        if (target.capture_count) c_format(c, "xrl_callable_captures_%" PRIu32, f + 1);
        else c_text(c, "NULL");
        c_format(c, ", %" PRIu32 "},\n", target.capture_count);
    }
    c_format(c, "};\nstatic const XrtCallableImage xrl_callable_image = {xrl_callable_types, %" PRIu32
        ", xrl_callable_entries, %" PRIu32 "};\n", count, c->owned_entry_count);
    xr_compile_resources_free(parameters);
}
static bool c_callable_captured_type(const LirC *c, XrLirType type) {
    for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
        if (!c->owned_entries[f]) continue;
        XrLirClosureTarget target;
        XrLirFunctionView function;
        XrLirFunction handle = {c->request->module, f + 1};
        xr_lir_closure_target_view(c->request->module, handle, &target);
        xr_lir_function_view(c->request->module, handle, &function);
        for (uint32_t p = 0; p < target.capture_count; ++p)
            if (function.parameter_types[p] == type) return true;
    }
    return false;
}
static void c_callable_import_declarations(LirC *c) {
    for (uint32_t t = 0; c->owned_entry_count && t < c->constructed_count; ++t) {
        XrLirCallableType signature;
        XrLirType type = XR_LIR_TYPE_COUNT + t;
        if (xr_lir_callable_type_view(c->request->module, type, &signature) == XR_LIR_OK && c_callable_captured_type(c, type))
            c_format(c, "static %s xrl_callable_import_%" PRIu32 "(XrtCallable *, uint32_t);\n", c_type(c, type), type);
    }
}
static bool c_callable_owned_thunk_needed(LirC *c, uint32_t target) {
    if (c->seen[target] & 32u) return true;
    XrLirClosureTarget callee = c->request->module->functions[target].closure;
    for (uint32_t f = 0; f < c->request->module->function_count; ++f) {
        if (!c->seen[f]) continue;
        const XrLirFunctionData *function = &c->request->module->functions[f];
        if (c->owned_entries[f])
            for (uint32_t p = 0; p < function->closure.capture_count; ++p)
                if (xr_lir_callable_accepts(c->request->module, function->parameter_types[p], callee.callable)) return true;
        for (uint32_t b = 0; b < function->block_count; ++b)
            for (uint32_t i = 0; i < function->blocks[b].instruction_count; ++i) {
                const XrLirInstructionView *v = &function->blocks[b].instructions[i].view;
                if (v->op == XR_LIR_STATE_READ && xr_lir_callable_accepts(c->request->module, v->type, callee.callable))
                    return true;
            }
    }
    return false;
}
static void c_callable_owned_thunks(LirC *c) {
    for (uint32_t f = 0; c->owned_entry_count && f < xr_lir_function_count(c->request->module); ++f) {
        if (!c->owned_entries[f] || !c_callable_owned_thunk_needed(c, f)) continue;
        XrLirClosureTarget target;
        XrLirFunctionView view;
        XrLirCallableType signature;
        XrLirFunction function = {c->request->module, f + 1};
        xr_lir_closure_target_view(c->request->module, function, &target);
        xr_lir_function_view(c->request->module, function, &view);
        xr_lir_callable_type_view(c->request->module, target.callable, &signature);
        c_format(c, "static %s xrl_owned_thunk_%" PRIu32 "(void *environment", c_type(c, signature.result), f + 1);
        for (uint32_t p = 0; p < signature.parameter_count; ++p)
            c_format(c, ", %s a%" PRIu32, c_type(c, signature.parameters[p].type), p);
        c_text(c, ") {\n");
        if (!target.capture_count) c_text(c, "    (void)environment;\n");
        for (uint32_t p = 0; p < target.capture_count; ++p) {
            c_format(c, "    uint64_t bits%" PRIu32 " = xrt_callable_capture(&xrl_callable_image, environment, %" PRIu32
                ", %" PRIu32 ", 0);\n", p, c->owned_entries[f] - 1, p);
            XrLirType type = view.parameter_types[p];
            XrLirCallableType captured;
            if (xr_lir_callable_type_view(c->request->module, type, &captured) == XR_LIR_OK)
                c_format(c, "    %s c%" PRIu32 " = xrl_callable_import_%" PRIu32 "((XrtCallable *)(uintptr_t)bits%" PRIu32 ", 0);\n",
                    c_type(c, type), p, type, p);
            else if (type == XR_LIR_I1 || type == XR_LIR_U8)
                c_format(c, "    %s c%" PRIu32 " = (%s)bits%" PRIu32 ";\n", c_type(c, type), p, c_type(c, type), p);
            else c_format(c, "    %s c%" PRIu32 ";\n    memcpy(&c%" PRIu32 ", &bits%" PRIu32 ", sizeof(c%" PRIu32 "));\n",
                c_type(c, type), p, p, p, p);
        }
        c_text(c, "    ");
        if (signature.result != XR_LIR_UNIT) c_text(c, "return ");
        c_function_name(c, f + 1);
        c_text(c, "(");
        for (uint32_t p = 0; p < target.capture_count; ++p) c_format(c, p ? ", c%" PRIu32 : "c%" PRIu32, p);
        for (uint32_t p = 0; p < signature.parameter_count; ++p)
            c_format(c, p || target.capture_count ? ", a%" PRIu32 : "a%" PRIu32, p);
        c_text(c, ");\n}\n");
    }
}
static void c_callable_owned_reference(LirC *c, const XrLirInstructionView *v) {
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    c_text(c, "    {\n");
    if (v->operand_count) {
        c_format(c, "    uint64_t captures[%" PRIu32 "] = {0};\n", v->operand_count);
        for (uint32_t p = 0; p < v->operand_count; ++p) {
            XrLirType type = c_value_type(c, v->operands[p]);
            XrLirCallableType captured;
            if (xr_lir_callable_type_view(c->request->module, type, &captured) == XR_LIR_OK) {
                c_format(c, "    captures[%" PRIu32 "] = (uint64_t)(uintptr_t)xrl_state_function_%" PRIu32 "(", p, type);
                c_value(c, v->operands[p]);
                c_format(c, ", xrl_scope, %" PRIu32 ");\n", source);
            } else if (type == XR_LIR_I1 || type == XR_LIR_U8) {
                c_format(c, "    captures[%" PRIu32 "] = ", p);
                c_value(c, v->operands[p]);
                c_text(c, ";\n");
            } else {
                c_format(c, "    memcpy(&captures[%" PRIu32 "], &", p);
                c_value(c, v->operands[p]);
                c_text(c, ", sizeof(uint64_t));\n");
            }
        }
    }
    c_format(c, "    XrtCallable *owned = xrt_callable_scope_new(xrl_scope, %" PRIu32 ", ", c->owned_entries[v->callee - 1] - 1);
    c_text(c, v->operand_count ? "captures" : "NULL");
    c_format(c, ", %" PRIu32 ", %" PRIu32 ");\n    v%" PRIu32 " = (%s){xrl_owned_thunk_%" PRIu32
        ", owned, owned};\n    }\n", v->operand_count, source, v->result, c_type(c, v->type), v->callee);
}
static bool c_callable_state_type(const LirC *c, XrLirType type) {
    const XrLirModule *module = c->request->module;
    if (!c->state_layout.slot_count) return false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!c->seen[f]) continue;
        const XrLirFunctionData *function = &module->functions[f];
        for (uint32_t b = 0; b < function->block_count; ++b)
            for (uint32_t i = 0; i < function->blocks[b].instruction_count; ++i) {
                const XrLirInstructionView *v = &function->blocks[b].instructions[i].view;
                if (v->operand_count != 2) continue;
                if ((v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_INITIALIZE) &&
                    function->values[v->operands[1] - 1].type == type) return true;
                if (v->op == XR_LIR_STATE_WRITE) {
                    XrLirStateReference shape = {0};
                    if (xr_lir_state_reference_view(module,
                        (XrLirValue){module, f + 1, v->operands[0]}, &shape) == XR_LIR_OK &&
                        shape.payload == type) return true;
                }
            }
    }
    return false;
}
static void c_callable_state_exports(LirC *c) {
    if (!c->owned_entry_count) return;
    for (uint32_t t = 0; t < c->constructed_count; ++t) {
        XrLirType type = XR_LIR_TYPE_COUNT + t;
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(c->request->module, type, &signature) != XR_LIR_OK) continue;
        bool captured = c_callable_captured_type(c, type);
        if (!captured && !c_callable_state_type(c, type)) continue;
        c_format(c, "static XrtCallable *xrl_state_function_%" PRIu32 "(%s value, XrtCallableScope *scope, uint32_t source) {\n",
            type, c_type(c, type));
        c_text(c, "    (void)scope;\n    if (value.owner) return value.owner;\n"
            "    if (value.environment) xrt_module_uninitialized(source);\n");
        for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
            if (!c->owned_entries[f] || !(c->seen[f] & 2u)) continue;
            XrLirClosureTarget target;
            xr_lir_closure_target_view(c->request->module, (XrLirFunction){c->request->module, f + 1}, &target);
            if (target.capture_count || target.state_capture || !xr_lir_callable_accepts(c->request->module, type, target.callable)) continue;
            c_format(c, "    if (value.invoke == xrl_thunk_%" PRIu32 ") return xrt_callable_scope_new(scope, %" PRIu32
                ", NULL, 0, source);\n", f + 1, c->owned_entries[f] - 1);
        }
        c_text(c, "    xrt_module_uninitialized(source);\n}\n");
        if (!captured) continue;
        c_format(c, "static %s xrl_callable_import_%" PRIu32 "(XrtCallable *owned, uint32_t source) {\n", c_type(c, type), type);
        c_text(c, "    switch (xrt_callable_entry(&xrl_callable_image, owned, source)) {\n");
        for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
            if (!c->owned_entries[f]) continue;
            XrLirClosureTarget target;
            xr_lir_closure_target_view(c->request->module, (XrLirFunction){c->request->module, f + 1}, &target);
            if (xr_lir_callable_accepts(c->request->module, type, target.callable))
                c_format(c, "    case %" PRIu32 ": return (%s){xrl_owned_thunk_%" PRIu32 ", owned, owned};\n",
                    c->owned_entries[f] - 1, c_type(c, type), f + 1);
        }
        c_text(c, "    default: xrt_module_uninitialized(source);\n    }\n}\n");
    }
}
