/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_array_region.inc.c - Stable typed access identities for common regions
 */
static bool rbc_lir_fixed_array_region(RbcLirCompiler *c, const XrLirFixedArrayUpdateHome *row, uint32_t id) {
    const XrLirArraySetRegion *region = &row->region;
    const XrLirValueHomes *homes = c->homes;
    if (region->first_step > homes->fixed_array_update_step_count ||
        region->step_count > homes->fixed_array_update_step_count - region->first_step)
        return false;
    for (uint32_t n = 0; n < region->step_count; ++n) {
        const XrLirArraySetRegionStep *step = &homes->fixed_array_update_steps[region->first_step + n];
        if (!step->edge) continue;
        if (!step->block || step->block > c->view.block_count || step->edge > 2) return false;
        RbcLirFixedArrayBlock *block = &c->fixed_array_blocks[step->block - 1];
        RbcLirFixedArrayEdge *edge = &block->edges[step->edge - 1];
        XrLirArrayRegionEdgeKind kind;
        switch (step->kind) {
        case XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE:
            if (edge->entry_region || (edge->region && edge->kind != XR_LIR_ARRAY_REGION_EDGE_EXIT)) return false;
            edge->entry_region = id;
            continue;
        case XR_LIR_ARRAY_SET_REGION_EXIT_MOVE: kind = XR_LIR_ARRAY_REGION_EDGE_EXIT; break;
        case XR_LIR_ARRAY_SET_REGION_EDGE_MOVE:
            if (block->region && block->region != id) return false;
            block->region = id;
            if (step->block == region->latch) kind = XR_LIR_ARRAY_REGION_EDGE_LATCH;
            else if (step->block == region->header && step->edge == row->body_edge) kind = XR_LIR_ARRAY_REGION_EDGE_BODY;
            else continue;
            break;
        default: return false;
        }
        if (edge->region || (edge->entry_region && kind != XR_LIR_ARRAY_REGION_EDGE_EXIT)) return false;
        edge->region = id;
        edge->kind = kind;
    }
    return true;
}

static bool rbc_lir_array_scalar_location(XrRbcValueLocation location) {
    return location.kind==XR_RBC_VALUE_REGISTER || location.kind==XR_RBC_VALUE_SPILL;
}

/* The single common route determines whether this lowering can skip scalar
 * replay. Project only its bound and physical header IV at the real latch. */
static bool rbc_lir_counted_push_prepare(RbcLirCompiler *c) {
    if (!c->homes || !c->homes->fixed_array_update_count) return true;
    XrRbcValueCode *code = c->value_code;
    XrRbcArrayCountedPush *counted = NULL;
    for (uint32_t n = 0; n < c->homes->fixed_array_update_count; ++n) {
        const XrLirArraySetRegion *region = &c->homes->fixed_array_updates[n].region;
        if (!region->push_counted) continue;
        XrLirArraySetRegionRange range = region->range;
        if (region->primary_kind != XR_LIR_ARRAY_REGION_PRIMARY_PUSH ||
            range.kind != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT ||
            !range.iv || range.iv > code->value_count || !range.length || range.length > code->value_count)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "counted append lost common Array facts");
        if (!rbc_lir_array_scalar_location(code->locations[range.iv])) continue;
        /* An invariant header parameter has already been republished; a
         * pre-loop definition is live here by ordinary CFG liveness. Read
         * constant bits directly, even if the emitter used an immediate. */
        const XrLirFunctionData *function = &c->input->functions[c->function.id - 1];
        const XrLirInstructionView *end = xr_lir_internal_value_definition(function, range.length);
        if (end && end->type != XR_LIR_I64)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "counted append bound is not an i64");
        bool constant = end && end->op == XR_LIR_CONST;
        if (!constant && !rbc_lir_array_scalar_location(code->locations[range.length])) continue;
        if (!counted) {
            counted = xr_calloc(c->homes->fixed_array_update_count, sizeof(*counted));
            if (!counted)
                return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own counted append bounds");
            code->array_counted_push = counted;
        }
        counted[n] = (XrRbcArrayCountedPush){.iv=range.iv, .limit=constant ? 0 : range.length,
            .bits=constant ? end->bits : 0};
    }
    return true;
}

static bool rbc_lir_array_region_prepare(RbcLirCompiler *c) {
    const XrLirValueHomes *homes = c->homes;
    if (!homes || !homes->fixed_array_update_count) return true;
    if (!c->view.block_count ||
        sizeof(*c->fixed_array_blocks) > SIZE_MAX / c->view.block_count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "fixed Array regions exceed bytecode capacity");
    c->fixed_array_blocks = xr_calloc(c->view.block_count, sizeof(*c->fixed_array_blocks));
    if (!c->fixed_array_blocks)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot index fixed Array regions");
    for (uint32_t n = 0; n < homes->fixed_array_update_count; ++n) {
        if (!rbc_lir_fixed_array_region(c, &homes->fixed_array_updates[n], n + 1))
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "fixed Array region lost its selected edges");
    }
    c->value_code->array_region_runtime = true;
    return true;
}

static bool rbc_lir_array_read_ready(const XrRbcValueCode *code, const XrLirArraySetRegion *region) {
    const XrLirArrayReadSum *sum = &region->read_sum;
    return sum->accumulator && rbc_lir_array_scalar_location(code->locations[region->range.iv]) &&
        rbc_lir_array_scalar_location(code->locations[sum->accumulator]) &&
        (!sum->limit || rbc_lir_array_scalar_location(code->locations[sum->limit]));
}

static void rbc_lir_array_region_role(RbcLirCompiler *c, const XrRbcValueAccess *access, XrRbcArrayRegionAccess *role) {
    if (c->fixed_array_marker.region) {
        role->region = c->fixed_array_marker.region;
        role->fixed_edge = (uint8_t)c->fixed_array_marker.kind;
        if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_LATCH)
            role->sum_checkpoint = rbc_lir_array_read_ready(c->value_code, &c->homes->fixed_array_updates[role->region - 1].region);
        return;
    }
    uint32_t region = c->fixed_array_blocks[c->diagnostic->block - 1].region;
    if (region && (access->operation == XR_RBC_VALUE_EDGE || access->operation == XR_RBC_VALUE_ARRAY ||
            access->operation == XR_RBC_VALUE_BINDING_NEW || access->operation == XR_RBC_VALUE_BINDING_READ)) {
        role->region = region;
        role->binding_identity = rbc_value_binding_identity(c->value_code, *access);
    }
}
static bool rbc_lir_array_region_append(RbcLirCompiler *c,const XrRbcValueAccess *access) {
    XrRbcValueCode *code=c->value_code;
    if (!code->array_region_runtime) return true;
    XrRbcArrayRegionAccess role={0};
    rbc_lir_array_region_role(c, access, &role);
    void *storage=(void *)code->array_region_accesses;
    if (!rbc_lir_grow(c,&storage,&c->value_region_access_capacity,code->access_count+1,sizeof(role))) return false;
    code->array_region_accesses=storage;
    ((XrRbcArrayRegionAccess *)storage)[code->access_count]=role;
    return true;
}
