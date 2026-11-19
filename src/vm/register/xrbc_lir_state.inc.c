/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_state.inc.c - Encode State accesses from common receiving plans
 */
static bool rbc_lir_state_payload(RbcLirCompiler *c, XrLirType type, XrLirType element, XrtValueKind *kind) {
    const XrtValuePlan *plan = NULL; uint32_t id = 0;
    XrLirValueLayoutKey key = {type,type == XR_LIR_ARRAY ? element : 0,0};
    if (!c->literals->layouts || xr_lir_value_layouts_lookup(c->literals->layouts,key,&plan,&id) != XR_LIR_VALUE_LAYOUT_OK)
        return false;
    XrtValuePlanSpec spec;
    if (xrt_value_plan_view(plan,&spec) != XRT_MANAGED_OK) return false;
    *kind = spec.kind; return true;
}
static bool rbc_lir_state_instruction(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t first = c->slots[i->operands[0]], result = i->result ? c->slots[i->result] : 0;
    if (i->op == XR_LIR_MODULE_BEGIN || i->op == XR_LIR_MODULE_READY)
        return rbc_lir_emit(c, i->op == XR_LIR_MODULE_BEGIN ? XR_RBC_MODULE_BEGIN : XR_RBC_MODULE_READY,
            first, 0, 0, (int64_t)i->bits);
    if (i->op == XR_LIR_STATE_PUBLISH) return rbc_lir_emit(c, XR_RBC_STATE_PUBLISH, first, 0, 0, 0);
    if (i->op == XR_LIR_STATE_READ || i->op == XR_LIR_STATE_WRITE) {
        XrLirStateReference shape = {0};
        XrtValueKind kind;
        if (xr_lir_state_reference_view(c->input,
            (XrLirValue){c->input, c->function.id, i->operands[0]}, &shape) != XR_LIR_OK ||
            !rbc_lir_state_payload(c, shape.payload, shape.array_element, &kind))
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
