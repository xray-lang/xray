/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_state.inc.c - Receive owned LIR state layouts and reference shapes
 */
static bool rbc_lir_state_payload(XrLirType type, XrLirType element, XrtModuleTypeKind *kind, uint32_t *index) {
    *kind = lir_state_payload_kind(type, element);
    if (!*kind) return false;
    *index = (uint32_t)*kind - XRT_MODULE_UNIT;
    return true;
}

static XrRbcCompileStatus rbc_lir_state_layout(const XrLirModule *input, XrLirFunction entry,
    XrRbcProgram *program, XrRbcCompileDiagnostic *diagnostic) {
    XrLirFunction state_entry = {0};
    XrLirStateLayout layout = {0};
    if (xr_lir_state_entry(input, &state_entry) != XR_LIR_OK ||
        xr_lir_state_layout_view(input, &layout) != XR_LIR_OK)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_INVALID, "cannot read owned module layout", (XrLirSource){0});
    if (!state_entry.id || state_entry.id != entry.id) return XR_RBC_COMPILE_OK;
    XrtModuleSpec *modules = layout.module_count ? xr_malloc((size_t)layout.module_count * sizeof(*modules)) : NULL;
    XrtModuleSlotSpec *slots = layout.slot_count ? xr_malloc((size_t)layout.slot_count * sizeof(*slots)) : NULL;
    XrRbcCompileStatus status = XR_RBC_COMPILE_OK;
    if ((layout.module_count && !modules) || (layout.slot_count && !slots)) status = XR_RBC_COMPILE_OUT_OF_MEMORY;
    for (uint32_t n = 0; status == XR_RBC_COMPILE_OK && n < layout.module_count; ++n)
        modules[n] = (XrtModuleSpec){layout.modules[n].first_dependency, layout.modules[n].dependency_count};
    for (uint32_t n = 0; status == XR_RBC_COMPILE_OK && n < layout.slot_count; ++n) {
        XrLirStateSlot source = layout.slots[n];
        XrtModuleTypeKind kind;
        uint32_t type;
        if (source.payload == XR_LIR_VIEW_CELL) {
            type = LIR_STATE_CONSTRUCTED_BASE + (uint32_t)program->module.aggregate_type_count;
        } else if (source.payload >= XR_LIR_TYPE_COUNT) {
            uint32_t ordinal = source.payload - XR_LIR_TYPE_COUNT;
            if (program->physical_aggregates) ordinal = program->physical_aggregates[ordinal];
            if (ordinal >= program->module.aggregate_type_count || ordinal > UINT32_MAX - LIR_STATE_CONSTRUCTED_BASE) {
                status = XR_RBC_COMPILE_UNSUPPORTED; break;
            }
            type = LIR_STATE_CONSTRUCTED_BASE + ordinal;
        } else if (!rbc_lir_state_payload(source.payload, source.array_element, &kind, &type)) {
            status = XR_RBC_COMPILE_UNSUPPORTED; break;
        }
        slots[n] = (XrtModuleSlotSpec){source.module, type,
            source.mutable ? XRT_MODULE_SLOT_MUTABLE : 0, source.offset, source.size, source.alignment};
    }
    if (status == XR_RBC_COMPILE_OK) {
        uint64_t type_count = (uint64_t)LIR_STATE_CONSTRUCTED_BASE + program->module.aggregate_type_count +
            lir_state_has_view_cells(&layout);
        XrtModuleTypeSpec *types = NULL;
        if (type_count > UINT32_MAX / 2 || type_count > SIZE_MAX / sizeof(*types)) status = XR_RBC_COMPILE_LIMIT;
        else if (!(types = xr_malloc((size_t)type_count * sizeof(*types)))) status = XR_RBC_COMPILE_OUT_OF_MEMORY;
        if (types) {
            for (uint32_t kind = XRT_MODULE_UNIT; kind < XRT_MODULE_FIXED_U8; ++kind)
                types[kind - XRT_MODULE_UNIT] = (XrtModuleTypeSpec){(XrtModuleTypeKind)kind, 0};
            for (size_t t = 0; t < program->module.aggregate_type_count; ++t) {
                XrPodType shape = program->module.aggregate_types[t];
                uint32_t argument = shape.count;
                if (shape.kind == XR_POD_NULLABLE) argument = shape.element < XR_POD_SCALAR_COUNT ?
                    (uint32_t)xr_pod_module_kind((XrPodScalar)shape.element) - XRT_MODULE_UNIT :
                    LIR_STATE_CONSTRUCTED_BASE + shape.element - XR_POD_SCALAR_COUNT;
                types[LIR_STATE_CONSTRUCTED_BASE + t] = (XrtModuleTypeSpec){
                    shape.kind == XR_POD_NULLABLE ? XRT_MODULE_NULLABLE : XRT_MODULE_FIXED_U8, argument};
            }
            if (lir_state_has_view_cells(&layout)) types[type_count - 1] =
                (XrtModuleTypeSpec){XRT_MODULE_CELL_ARRAY_I64, 0};
        }
        XrtModuleLayoutSpec spec = {types, (uint32_t)type_count, modules, layout.module_count, layout.dependencies,
            layout.dependency_count, slots, layout.slot_count, layout.payload_size, UINT64_MAX,
            layout.payload_alignment};
        XrXirValueStatus copied = status == XR_RBC_COMPILE_OK ?
            xrt_module_layout_new(&spec, &program->module.state_layout) : XR_XIR_VALUE_OK;
        if (copied != XR_XIR_VALUE_OK) status = copied == XR_XIR_VALUE_OOM ?
            XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID;
        xr_free(types);
    }
    xr_free(modules); xr_free(slots);
    return status == XR_RBC_COMPILE_OK ? status :
        rbc_lir_error(diagnostic, status, "cannot receive module state layout", (XrLirSource){0});
}

static bool rbc_lir_state_instruction(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t first = c->slots[i->operands[0]], result = i->result ? c->slots[i->result] : 0;
    if (i->op == XR_LIR_MODULE_BEGIN || i->op == XR_LIR_MODULE_READY)
        return rbc_lir_emit(c, i->op == XR_LIR_MODULE_BEGIN ? XR_RBC_MODULE_BEGIN : XR_RBC_MODULE_READY,
            first, 0, 0, (int64_t)i->bits);
    if (i->op == XR_LIR_STATE_PUBLISH) return rbc_lir_emit(c, XR_RBC_STATE_PUBLISH, first, 0, 0, 0);
    if (i->op == XR_LIR_STATE_READ || i->op == XR_LIR_STATE_WRITE) {
        XrLirStateReference shape = {0};
        XrtModuleTypeKind kind;
        uint32_t unused;
        if (xr_lir_state_reference_view(c->input,
            (XrLirValue){c->input, c->function.id, i->operands[0]}, &shape) != XR_LIR_OK ||
            !rbc_lir_state_payload(shape.payload, shape.array_element, &kind, &unused))
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "module reference payload is not admitted");
        return i->op == XR_LIR_STATE_READ ? rbc_lir_emit(c, XR_RBC_STATE_READ, result, first, kind, 0) :
            rbc_lir_emit(c, XR_RBC_STATE_WRITE, first, i->operand_count == 2 ? c->slots[i->operands[1]] : 0, kind, 0);
    }
    if (c->state_access_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "module state access table exceeds encoding");
    void *storage = c->state_accesses;
    if (!rbc_lir_grow(c, &storage, &c->state_access_capacity, c->state_access_count + 1,
        sizeof(*c->state_accesses))) return false;
    c->state_accesses = storage;
    uint32_t index = (uint32_t)c->state_access_count++;
    c->state_accesses[index] = (XrRbcStateAccess){(uint32_t)i->bits,
        i->op == XR_LIR_STATE_INITIALIZE ? 0 : first, i->operand_count == 2 ? c->slots[i->operands[1]] : 0};
    XrRbcOp op = i->op == XR_LIR_STATE_CONSTRUCT ? XR_RBC_STATE_CONSTRUCT :
        i->op == XR_LIR_STATE_INITIALIZE ? XR_RBC_STATE_INITIALIZE : XR_RBC_STATE_REFERENCE;
    return rbc_lir_emit(c, op, i->op == XR_LIR_STATE_INITIALIZE ? first : result, 0, 0, index);
}
