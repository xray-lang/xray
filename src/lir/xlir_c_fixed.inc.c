/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_fixed.inc.c - Exact byte aggregates with ordinary C value transport
 *
 * KEY CONCEPT:
 *   Each physical type has one struct definition. Calls, returns and selected
 *   edges copy that value. State storage has byte alignment and is read with
 *   memcpy, while extraction checks bounds before indexing the actual value.
 */
static const char *c_type(const LirC *c, XrLirType type) {
    if (type < XR_LIR_TYPE_COUNT) return c_builtin_type(type);
    uint32_t index = type - XR_LIR_TYPE_COUNT;
    return index < c->constructed_count ? c->constructed_types[index].name : NULL;
}

static void c_prepare_types(LirC *c) {
    const XrLirModule *module = c->request->module;
    if (!xr_lir_module_is_readable(module)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "C emission requires a finished physical type pool");
        return;
    }
    c->state_type_base = LIR_STATE_CONSTRUCTED_BASE;
    uint32_t count = xr_lir_constructed_type_count(module);
    if (!count) return;
    if (sizeof(*c->constructed_types) > SIZE_MAX / count) {
        c_error(c, XR_LIR_C_OVERFLOW, "C physical type inventory exceeds machine capacity");
        return;
    }
    void *memory = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
        count, sizeof(*c->constructed_types), &memory))) return;
    c->constructed_types = memory;
    c->constructed_count = count;
    bool cells = false;
    for (uint32_t n = 0; n < count && c_request_poll(c); ++n) {
        XrLirType type = XR_LIR_TYPE_COUNT + n;
        LirCConstructedType *owned = &c->constructed_types[n];
        owned->pod_index = UINT32_MAX;
        XrLirCellType cell;
        if (xr_lir_cell_type_view(module, type, &cell) == XR_LIR_OK && (cell.payload == XR_LIR_I64 || cell.payload == XR_LIR_MAP_STRING_I64)) {
            cells = true;
            xr_lir_type_view(module, type, &owned->shape);
            owned->layout = (XrLirTypeLayout){sizeof(void *), _Alignof(void *), 0, true};
            snprintf(owned->name, sizeof(owned->name), "XrlCell_%" PRIu32, type);
            continue;
        }
        if (xr_lir_type_view(module, type, &owned->shape) == XR_LIR_OK &&
            owned->shape.kind == XR_LIR_TYPE_CALLABLE) {
            owned->layout = (XrLirTypeLayout){24, 8, 0, true};
            snprintf(owned->name, sizeof(owned->name), "XrlFn_%" PRIu32, type);
            XrLirCallableType signature;
            if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK) {
                c_error(c, XR_LIR_C_INVALID_LIR, "callable signature is unavailable");
                return;
            }
            c_scalar(c, signature.result);
            for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p) {
                if (signature.parameters[p].escaping)
                    c_error(c, XR_LIR_C_UNSUPPORTED, "escaping callable parameters require owned environment transport");
                c_scalar(c, signature.parameters[p].type);
            }
            continue;
        }
        if (xr_lir_boxed_type(module, type)) {
            c->boxed = c->runtime = true;
            xr_lir_type_view(module, type, &owned->shape);
            xr_lir_type_layout(module, type, &owned->layout);
            snprintf(owned->name, sizeof(owned->name), "XrlValue_%" PRIu32, type);
            continue;
        }
        if (xr_lir_type_view(module, type, &owned->shape) != XR_LIR_OK ||
            xr_lir_type_layout(module, type, &owned->layout) != XR_LIR_OK ||
            (owned->shape.kind != XR_LIR_TYPE_FIXED_ARRAY && owned->shape.kind != XR_LIR_TYPE_NULLABLE &&
             owned->shape.kind != XR_LIR_TYPE_TUPLE) ||
            owned->layout.managed) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "constructed physical types have no native implementation");
            return;
        }
        snprintf(owned->name, sizeof(owned->name), owned->shape.kind == XR_LIR_TYPE_FIXED_ARRAY ? "XrlFixed_%" PRIu32 :
            owned->shape.kind == XR_LIR_TYPE_TUPLE ? "XrlTuple_%" PRIu32 : "XrlNullable_%" PRIu32, type);
        owned->pod_index = c->pod_count++;
    }
    for (uint32_t f = 0; cells && f < xr_lir_function_count(module) && c_request_poll(c); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        xr_lir_function_at(module, f, &function);
        xr_lir_function_view(module, function, &view);
        if (!lir_definition_cell_captures(module, function, &view))
            c_error(c, XR_LIR_C_UNSUPPORTED, "owned Cells require an actual hidden capture parameter");
    }
}

static void c_constructed_types(LirC *c) {
    for (uint32_t n = 0; n < c->constructed_count && c_request_poll(c); ++n) {
        const LirCConstructedType *type = &c->constructed_types[n];
        if (xr_lir_boxed_type(c->request->module, XR_LIR_TYPE_COUNT + n)) {
            c_format(c, "typedef XrtBoxed *%s;\n", type->name);
        } else if (type->shape.kind == XR_LIR_TYPE_CELL) {
            c_format(c, "typedef XrtCapturedCell *%s;\n", type->name);
        } else if (type->shape.kind == XR_LIR_TYPE_CALLABLE) {
            c_callable_type(c, XR_LIR_TYPE_COUNT + n);
        } else if (type->shape.kind == XR_LIR_TYPE_FIXED_ARRAY) {
            c_format(c, "typedef struct %s { uint8_t bytes[%" PRIu32 "]; } %s;\n",
                type->name, type->shape.count, type->name);
        } else if (type->shape.kind == XR_LIR_TYPE_TUPLE) {
            c_format(c, "typedef struct %s { int64_t fields[%" PRIu32 "]; } %s;\n",
                type->name, type->shape.count, type->name);
        } else {
            XrLirTypeLayout payload;
            if (xr_lir_type_layout(c->request->module, type->shape.element, &payload) != XR_LIR_OK) {
                c_error(c, XR_LIR_C_INVALID_LIR, "nullable payload layout is unavailable");
                return;
            }
            c_format(c, "typedef struct %s { uint8_t tag;", type->name);
            if (payload.alignment > 1) c_format(c, " uint8_t pad[%" PRIu32 "];", payload.alignment - 1);
            if (type->shape.element != XR_LIR_UNIT) c_format(c, " %s payload;",
                type->shape.element == XR_LIR_F32 ? "float" : c_type(c, type->shape.element));
            c_format(c, " } %s;\n", type->name);
            if (type->shape.element != XR_LIR_UNIT)
                c_format(c, "_Static_assert(offsetof(%s, payload) == %u, \"nullable payload offset\");\n",
                    type->name, payload.alignment);
        }
        c_format(c, "_Static_assert(sizeof(%s) == UINT64_C(%" PRIu64 "), \"aggregate size\");\n",
            type->name, type->layout.size);
        c_format(c, "_Static_assert(_Alignof(%s) == UINT32_C(%" PRIu32 "), \"aggregate alignment\");\n",
            type->name, type->layout.alignment);
    }
}

static void c_fixed_instruction(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_FIXED_ARRAY_NEW || v->op == XR_LIR_TUPLE_NEW) {
        c_format(c, "    v%" PRIu32 " = (%s){{", v->result, c_type(c, v->type));
        for (uint32_t n = 0; n < v->operand_count && c_request_poll(c); ++n) {
            if (n) c_text(c, ", ");
            c_value(c, v->operands[n]);
        }
        c_text(c, "}};\n");
        return;
    }
    if (v->op == XR_LIR_TUPLE_FIELD) {
        c_format(c, "    v%" PRIu32 " = ", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ".fields[UINT64_C(%" PRIu64 ")];\n", v->bits);
        return;
    }
    XrLirType type = c_value_type(c, v->operands[0]);
    const LirCConstructedType *fixed = &c->constructed_types[type - XR_LIR_TYPE_COUNT];
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    c_text(c, "    {\n    int64_t xrl_index = ");
    c_value(c, v->operands[1]);
    /* Keep valid loads ordinary C. Only the rejected branch captures through
     * volatile, so host propagation cannot diagnose subsequent guest code as
     * unreachable after a statically known panic. Both checks use one value. */
    c_format(c, ";\n    if ((uint64_t)xrl_index >= UINT64_C(%" PRIu32 ")) {\n"
        "        volatile int64_t xrl_guard = xrl_index;\n        xrl_index = xrl_guard;\n"
        "        if ((uint64_t)xrl_index >= UINT64_C(%" PRIu32 "))\n"
        "            xrt_fixed_array_bounds(UINT32_C(%" PRIu32 "));\n    }\n",
        fixed->shape.count,
        fixed->shape.count, source);
    c_format(c, "    v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, ".bytes[xrl_index];\n    }\n");
}
