/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_state.inc.c - Receive owned LIR state layouts and reference shapes
 */
static bool rbc_lir_state_payload(XrLirType type, XrtModuleTypeKind *kind, uint32_t *index) {
    switch (type) {
    case XR_LIR_UNIT: *kind = XRT_MODULE_UNIT; *index = 0; return true;
    case XR_LIR_I1: *kind = XRT_MODULE_BOOL; *index = 1; return true;
    case XR_LIR_I64: *kind = XRT_MODULE_I64; *index = 2; return true;
    case XR_LIR_F64: *kind = XRT_MODULE_F64; *index = 3; return true;
    case XR_LIR_STRING: *kind = XRT_MODULE_STRING; *index = 4; return true;
    default: return false;
    }
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
        if (!rbc_lir_state_payload(source.payload, &kind, &type)) {status = XR_RBC_COMPILE_UNSUPPORTED; break;}
        slots[n] = (XrtModuleSlotSpec){source.module, type,
            source.mutable ? XRT_MODULE_SLOT_MUTABLE : 0, source.offset, source.size, source.alignment};
    }
    if (status == XR_RBC_COMPILE_OK) {
        static const XrtModuleTypeSpec types[] = {{XRT_MODULE_UNIT}, {XRT_MODULE_BOOL},
            {XRT_MODULE_I64}, {XRT_MODULE_F64}, {XRT_MODULE_STRING}};
        XrtModuleLayoutSpec spec = {types, 5, modules, layout.module_count, layout.dependencies,
            layout.dependency_count, slots, layout.slot_count, layout.payload_size, UINT64_MAX,
            layout.payload_alignment};
        XrXirValueStatus copied = xrt_module_layout_new(&spec, &program->module.state_layout);
        if (copied != XR_XIR_VALUE_OK) status = copied == XR_XIR_VALUE_OOM ?
            XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID;
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
            !rbc_lir_state_payload(shape.payload, &kind, &unused))
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
