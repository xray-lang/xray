/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_spill_edges.inc.c - Whole selected-edge snapshots in typed spill banks
 */
static bool rbc_lir_spill_edge_type(RbcLirCompiler *c, uint32_t value) {
    XrLirType type = rbc_lir_value_type(c, value);
    return rbc_lir_scalar(type) || type == XR_LIR_MAP_STRING_I64 || type == XR_LIR_STRING || type == XR_LIR_ARRAY;
}

static bool rbc_lir_edge_spill_homes(RbcLirCompiler *c, bool managed) {
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            uint32_t value = view.parameters[p];
            XrLirType type = rbc_lir_value_type(c, value);
            if (!rbc_lir_spill_edge_type(c, value) ||
                ((type == XR_LIR_MAP_STRING_I64 || type == XR_LIR_STRING || type == XR_LIR_ARRAY) != managed)) continue;
            if (!c->edge_spill) {
                size_t count = (size_t)c->view.value_count + 1;
                if (count > SIZE_MAX / sizeof(*c->edge_spill))
                    return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "edge spill homes exceed address space");
                c->edge_spill = xr_malloc(count * sizeof(*c->edge_spill));
                if (!c->edge_spill)
                    return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own selected-edge spill homes");
                memset(c->edge_spill, 255, count * sizeof(*c->edge_spill));
            }
            if (c->spill_count == XR_RBC_SPILL_COUNT_MAX)
                return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "edge spill homes exceed sixteen-bit slot capacity");
            c->edge_spill[value] = c->spill_count++;
            if (managed) ++c->reference_spill_count;
        }
    }
    return true;
}

static bool rbc_lir_spilled_edge(RbcLirCompiler *c, XrLirEdgeView edge, const XrLirBlockView *target) {
    if (!c->spill_count) return false;
    for (uint32_t p = 0; p < edge.argument_count; ++p)
        if (rbc_lir_spill_edge_type(c, edge.arguments[p]) &&
            (c->spill_home[edge.arguments[p]] != UINT32_MAX || c->spill_home[target->parameters[p]] != UINT32_MAX))
            return true;
    return false;
}

static bool rbc_lir_edge_spill_copy(RbcLirCompiler *c, XrLirEdgeView edge,
    const XrLirBlockView *target, bool publish) {
    for (uint32_t p = 0; p < edge.argument_count; ++p) {
        uint32_t actual = edge.arguments[p], formal = target->parameters[p];
        if (!rbc_lir_spill_edge_type(c, actual)) continue;
        XrRbcRegisterKind kind = rbc_lir_register_kind(c, actual, rbc_lir_value_type(c, actual));
        uint32_t temporary = rbc_lir_copy_temporary(c, kind);
        if (temporary == UINT32_MAX) return false;
        XR_DCHECK(c->edge_spill && c->edge_spill[formal] < c->spill_count,
            "each scalar or managed edge parameter has its own typed snapshot home");
        if (publish) {
            if (!rbc_lir_emit(c, XR_RBC_RELOAD, temporary, 0, 0, c->edge_spill[formal])) return false;
            if (c->spill_home[formal] != UINT32_MAX) {
                if (!rbc_lir_emit(c, XR_RBC_SPILL, temporary, 0, 0, c->spill_home[formal])) return false;
            } else if (!rbc_lir_array_move(c, kind, c->slots[formal], temporary)) return false;
        } else {
            if (c->spill_home[actual] != UINT32_MAX) {
                if (!rbc_lir_emit(c, XR_RBC_RELOAD, temporary, 0, 0, c->spill_home[actual])) return false;
            } else if (!rbc_lir_array_move(c, kind, temporary, c->slots[actual])) return false;
            if (!rbc_lir_emit(c, XR_RBC_SPILL, temporary, 0, 0, c->edge_spill[formal])) return false;
        }
    }
    return true;
}
