/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_value_types.inc.c - Typed C declarations from the common receiving plans
 *
 * KEY CONCEPT:
 *   Field positions and owner leaves come from one immutable plan producer.
 *   Host C layout assertions check the declarations against those exact facts.
 */
typedef struct LirCValueTypeRow {
    const XrtValuePlan *plan;
    XrtValuePlanSpec spec;
    bool landing_declared;
} LirCValueTypeRow;
typedef struct LirCValueTypes {
    LirCValueTypeRow *rows;
    size_t count, *index, capacity;
} LirCValueTypes;

static size_t c_value_type_hash(const XrtValuePlan *plan) {
    uintptr_t bits = (uintptr_t)plan;
    bits ^= bits >> 17;
    bits *= (uintptr_t)UINT64_C(0x9e3779b97f4a7c15);
    return (size_t)(bits ^ (bits >> 11));
}
static size_t c_value_type_row(const LirCValueTypes *types, const XrtValuePlan *plan) {
    size_t at = c_value_type_hash(plan) & (types->capacity - 1);
    while (types->index[at]) {
        size_t row = types->index[at] - 1;
        if (types->rows[row].plan == plan) return row;
        at = (at + 1) & (types->capacity - 1);
    }
    return SIZE_MAX;
}
static void c_value_types_prepare(LirC *c, const XrLirValueLayouts *layouts, LirCValueTypes *types) {
    types->count = xr_lir_value_layouts_count(layouts);
    if (!types->count) return;
    size_t capacity = 2;
    if (types->count > SIZE_MAX / 2) {
        c_error(c, XR_LIR_C_OVERFLOW, "C value type inventory exceeds machine capacity");
        return;
    }
    while (capacity / 2 < types->count) {
        if (capacity > SIZE_MAX / 2) {
            c_error(c, XR_LIR_C_OVERFLOW, "C value type index exceeds machine capacity");
            return;
        }
        capacity *= 2;
    }
    void *rows = NULL, *index = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, types->count,
        sizeof(*types->rows), &rows))) return;
    types->rows = rows;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, capacity,
        sizeof(*types->index), &index))) return;
    types->index = index; types->capacity = capacity;
    for (size_t i = 0; i < types->count && c_request_poll(c); ++i) {
        XrLirValueLayoutKey key;
        LirCValueTypeRow *row = &types->rows[i];
        XR_CHECK(xr_lir_value_layouts_at(layouts, i, &key, &row->plan) == XR_LIR_VALUE_LAYOUT_OK &&
            xrt_value_plan_view(row->plan, &row->spec) == XRT_MANAGED_OK,
            "the receiving inventory owns every sealed physical description");
        size_t at = c_value_type_hash(row->plan) & (capacity - 1);
        while (types->index[at]) at = (at + 1) & (capacity - 1);
        types->index[at] = i + 1;
    }
}
static const char *c_value_scalar_type(XrtValueKind kind) {
    switch (kind) {
    case XRT_VALUE_UNIT: return "void";
    case XRT_VALUE_BOOL: return "_Bool";
    case XRT_VALUE_RUNE: return "uint32_t";
    case XRT_VALUE_I8: return "int8_t";
    case XRT_VALUE_I16: return "int16_t";
    case XRT_VALUE_I32: return "int32_t";
    case XRT_VALUE_I64: return "int64_t";
    case XRT_VALUE_U8: return "uint8_t";
    case XRT_VALUE_U16: return "uint16_t";
    case XRT_VALUE_U32: return "uint32_t";
    case XRT_VALUE_U64: return "uint64_t";
    case XRT_VALUE_F32: return "float";
    case XRT_VALUE_F64: return "double";
    case XRT_VALUE_FUNCTION: return "XrtFunction";
    case XRT_VALUE_STRING: case XRT_VALUE_ARRAY_I64: case XRT_VALUE_ARRAY_STRING:
    case XRT_VALUE_ARRAY_U8: case XRT_VALUE_ARRAY_U64: case XRT_VALUE_MAP_STRING_I64:
        return "XrtManagedHeader *";
    default: return NULL;
    }
}
static size_t c_value_dependency(LirC *c, const LirCValueTypes *types,
    const XrtValuePlan *dependency, size_t parent) {
    size_t row = c_value_type_row(types, dependency);
    if (row >= parent)
        c_error(c, XR_LIR_C_BAD_ARGUMENT, "C value type dependency is absent or out of order");
    return row;
}
static void c_value_type_declaration(LirC *c, const LirCValueTypes *types, size_t ordinal) {
    const XrtValuePlanSpec *spec = &types->rows[ordinal].spec;
    size_t type_id = ordinal + 1;
    const char *scalar = c_value_scalar_type(spec->kind);
    if (scalar) {
        c_format(c, "typedef %s XrlValue_%zu;\n", scalar, type_id);
    } else {
        c_format(c, "typedef struct XrlValue_%zu {\n", type_id);
        if (spec->kind == XRT_VALUE_RECORD) {
            if (!spec->field_count) c_text(c, "    uint8_t padding;\n");
            for (size_t f = 0; f < spec->field_count && c_request_poll(c); ++f) {
                size_t child = c_value_dependency(c, types, spec->fields[f].type, ordinal);
                c_format(c, "    XrlValue_%zu f%zu;\n", child + 1, f);
            }
        } else if (spec->kind == XRT_VALUE_NULLABLE) {
            size_t child = c_value_dependency(c, types, spec->nullable.element, ordinal);
            c_text(c, "    uint8_t tag;\n");
            if (c->status == XR_LIR_C_OK && types->rows[child].spec.kind != XRT_VALUE_UNIT)
                c_format(c, "    XrlValue_%zu payload;\n", child + 1);
        } else if (spec->kind == XRT_VALUE_FIXED_U8) {
            c_format(c, "    uint8_t bytes[%zu];\n", spec->count);
        } else if (spec->kind == XRT_VALUE_TUPLE_I64) {
            c_format(c, "    int64_t fields[%zu];\n", spec->count);
        } else {
            c_error(c, XR_LIR_C_UNSUPPORTED, "C value type requires a dedicated callable or capability ABI");
        }
        c_format(c, "} XrlValue_%zu;\n", type_id);
    }
    if (spec->kind == XRT_VALUE_UNIT) return;
    c_format(c, "_Static_assert(sizeof(XrlValue_%zu) == UINT64_C(%" PRIu64 "), \"value size\");\n",
        type_id, (uint64_t)spec->size);
    c_format(c, "_Static_assert(_Alignof(XrlValue_%zu) == UINT64_C(%" PRIu64 "), \"value alignment\");\n",
        type_id, (uint64_t)spec->alignment);
    for (size_t f = 0; f < spec->field_count && c_request_poll(c); ++f)
        c_format(c, "_Static_assert(offsetof(XrlValue_%zu, f%zu) == UINT64_C(%" PRIu64 "), \"field offset\");\n",
            type_id, f, (uint64_t)spec->fields[f].offset);
    if (spec->kind == XRT_VALUE_NULLABLE) {
        c_format(c, "_Static_assert(offsetof(XrlValue_%zu, tag) == UINT64_C(%" PRIu64 "), \"nullable tag\");\n",
            type_id, (uint64_t)spec->nullable.tag_offset);
        size_t child = c_value_dependency(c, types, spec->nullable.element, ordinal);
        if (c->status == XR_LIR_C_OK && types->rows[child].spec.kind != XRT_VALUE_UNIT)
            c_format(c, "_Static_assert(offsetof(XrlValue_%zu, payload) == UINT64_C(%" PRIu64 "), \"nullable payload\");\n",
                type_id, (uint64_t)spec->nullable.payload_offset);
    }
    if (spec->kind == XRT_VALUE_FUNCTION)
        c_format(c, "_Static_assert(offsetof(XrlValue_%zu, owner) == UINT64_C(%" PRIu64 "), \"function owner\");\n",
            type_id, (uint64_t)spec->owned_offsets[0]);
}

XR_FUNC XrLirCStatus xr_lir_emit_c_value_types(const XrLirCValueTypesRequest *request,
    XrLirCOutput *output, XrLirCDiagnostic *diagnostic) {
    if (diagnostic) *diagnostic = (XrLirCDiagnostic){0};
    XrLirCRequest common = {0};
    LirC c = {.request=&common, .diagnostic=diagnostic};
    if (!request || !request->layouts || !request->resources || !request->translation_unit ||
        !output || output->bytes || output->length || output->entry_result || output->entry_shape.kind ||
        output->entry_shape.element || output->entry_shape.count || output->entry_layout.size ||
        output->entry_layout.alignment || output->entry_layout.scalar_count || output->entry_layout.managed) {
        c_error(&c, XR_LIR_C_BAD_ARGUMENT, "C value types require receiving plans, resources and empty output");
        return c.status;
    }
    common.resources = request->resources; common.translation_unit = request->translation_unit;
    common.control = request->control;
    LirCValueTypes types = {0};
    if (c_request_poll(&c)) c_value_types_prepare(&c, request->layouts, &types);
    c_text(&c, "#include <stddef.h>\n#include <stdint.h>\n"
        "typedef struct XrtManagedHeader XrtManagedHeader;\n");
    for (size_t i = 0; i < types.count && c_request_poll(&c); ++i)
        if (types.rows[i].spec.kind == XRT_VALUE_FUNCTION) {
            c_text(&c, "#include \"xray_native_function.h\"\n");
            break;
        }
    for (size_t i = 0; i < types.count && c_request_poll(&c); ++i)
        c_value_type_declaration(&c, &types, i);
    c_verify_text(&c);
    if (c_request_poll(&c)) {
        *output = (XrLirCOutput){.bytes=c.text, .length=c.length};
        c.text = NULL;
    }
    xr_compile_resources_free(c.text);
    xr_compile_resources_free(types.index);
    xr_compile_resources_free(types.rows);
    return c.status;
}
