/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool lower_owned_binding_mark(Lower *c, uint32_t function, uint32_t value) {
    const XrXirFunction *body = &c->source->functions[function];
    uint64_t count = (uint64_t)body->parameter_count + body->instruction_count;
    if (value >= count) return lower_unsupported(c, "captured binding outside its actual function");
    if (!c->owned_bindings) {
        c->owned_bindings = lower_alloc(c, c->source->function_count, sizeof(*c->owned_bindings));
        if (!c->owned_bindings) return false;
    }
    if (!c->owned_bindings[function]) {
        c->owned_bindings[function] = lower_alloc(c, count, 1);
        if (!c->owned_bindings[function]) return false;
    }
    c->owned_bindings[function][value] = 1;
    return true;
}
static bool lower_owned_bindings(Lower *c) {
    if (!c->scalar_bindings) return true;
    bool maps = false;
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        if (!c->active[f]) continue;
        const XrXirFunction *body = &c->source->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF || !op->args[1]) continue;
            XirClosureStorage placement;
            if (xr_xir_compile_closure_placement(c->request->closed, f, body->parameter_count + i, &placement) != XR_XIR_OK)
                return lower_unsupported(c, "captured binding lost its constructor placement");
            if (placement != XIR_CLOSURE_OWNED) continue;
            const XrXirFunction *target = &c->source->functions[op->immediate];
            for (uint32_t p = 0; p < op->args[1]; ++p) {
                XrLirType type = lower_type(c, target->parameters[p]);
                if (type != XR_LIR_I64_CELL && type != XR_LIR_MAP_STRING_I64_CELL) continue;
                maps |= type == XR_LIR_MAP_STRING_I64_CELL;
                uint32_t value = body->operands[op->args[0] + p];
                for (uint32_t depth = 0;; ++depth) {
                    if (depth >= body->instruction_count || value < body->parameter_count ||
                        value - body->parameter_count >= body->instruction_count)
                        return lower_unsupported(c, "owned captured binding requires its actual local Cell constructor");
                    const XrXirInstruction *source = &body->instructions[value - body->parameter_count];
                    if (source->op != XR_XIR_COPY && source->op != XR_XIR_CELL_NEW)
                        return lower_unsupported(c, "owned captured binding has no local Cell identity");
                    if (!lower_owned_binding_mark(c, f, value)) return false;
                    if (source->op == XR_XIR_CELL_NEW) break;
                    value = source->args[0];
                }
                if (!lower_owned_binding_mark(c, (uint32_t)op->immediate, p)) return false;
            }
        }
    }
    if (!c->owned_bindings) return true;
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        if (!c->active[f] || !c->owned_bindings[f]) continue;
        const XrXirFunction *body = &c->source->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op == XR_XIR_COPY && op->args[0] < body->parameter_count + i && c->owned_bindings[f][op->args[0]])
                c->owned_bindings[f][body->parameter_count + i] = 1;
            if (op->op != XR_XIR_FUNCTION_REF) continue;
            for (uint32_t p = 0; p < op->args[1]; ++p)
                if (c->owned_bindings[f][body->operands[op->args[0] + p]] &&
                    !lower_owned_binding_mark(c, (uint32_t)op->immediate, p)) return false;
        }
    }
    if (!lower_build(c, xr_lir_cell_type(c->module, (XrLirCellType){XR_LIR_I64, 0}, &c->owned_i64_cell))) return false;
    return !maps || lower_build(c, xr_lir_cell_type(c->module,
        (XrLirCellType){XR_LIR_MAP_STRING_I64, 0}, &c->owned_map_cell));
}
static XrLirType lower_owned_cell_type(const Lower *c, XrXirType type) {
    XrXirType payload = xr_xir_cell_element(c->source->types, type);
    return lower_map_type(c, payload) ? c->owned_map_cell : c->owned_i64_cell;
}
