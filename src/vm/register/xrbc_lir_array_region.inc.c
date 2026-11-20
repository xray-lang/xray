/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_array_region.inc.c - Stable typed access identities for common regions
 */
static bool rbc_lir_array_region_uses_runtime(const XrLirArrayRegionBindings *bindings) {
    for (size_t n=0;n<bindings->region_count;++n) {
        const XrLirArrayRegionBinding *row=&bindings->regions[n];
        if (row->slot_count && row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET) return true;
    }
    return false;
}

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

static bool rbc_lir_fixed_array_prepare(RbcLirCompiler *c) {
    const XrLirValueHomes *homes = c->homes;
    if (!homes || !homes->fixed_array_update_count) return true;
    size_t legacy = rbc_value_legacy_array_region_count(c->value_code);
    if (legacy > UINT32_MAX - homes->fixed_array_update_count || !c->view.block_count ||
        sizeof(*c->fixed_array_blocks) > SIZE_MAX / c->view.block_count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "fixed Array regions exceed bytecode capacity");
    c->fixed_array_blocks = xr_calloc(c->view.block_count, sizeof(*c->fixed_array_blocks));
    if (!c->fixed_array_blocks)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot index fixed Array regions");
    for (uint32_t n = 0; n < homes->fixed_array_update_count; ++n) {
        if (!rbc_lir_fixed_array_region(c, &homes->fixed_array_updates[n], (uint32_t)legacy + n + 1))
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "fixed Array region lost its selected edges");
    }
    c->value_code->array_region_runtime = true;
    return true;
}

static bool rbc_lir_array_region_prepare(RbcLirCompiler *c) {
    if (!c->homes || !c->homes->array_region_count) return rbc_lir_fixed_array_prepare(c);
    XrLirValueLayoutStatus status=xr_lir_array_region_bindings_create(c->homes,c->literals->layouts,
        &c->value_code->array_regions);
    if (status==XR_LIR_VALUE_LAYOUT_OK) {
        c->value_code->array_region_runtime=rbc_lir_array_region_uses_runtime(c->value_code->array_regions);
        return rbc_lir_fixed_array_prepare(c);
    }
    XrRbcCompileStatus compile_failure=status==XR_LIR_VALUE_LAYOUT_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY :
        status==XR_LIR_VALUE_LAYOUT_OVERFLOW ? XR_RBC_COMPILE_LIMIT : XR_RBC_COMPILE_INVALID;
    return rbc_lir_fail(c,compile_failure,"cannot project common Array region coordinates");
}
static bool rbc_lir_array_region_role(RbcLirCompiler *c,const XrRbcValueAccess *access,XrRbcArrayRegionAccess *role) {
    if (c->fixed_array_marker.region) {
        role->region = c->fixed_array_marker.region;
        role->fixed_edge = (uint8_t)c->fixed_array_marker.kind;
        role->fixed = true;
        return true;
    }
    if (c->fixed_array_blocks) {
        uint32_t region = c->fixed_array_blocks[c->diagnostic->block - 1].region;
        if (region && (access->operation == XR_RBC_VALUE_EDGE || access->operation == XR_RBC_VALUE_ARRAY ||
                access->operation == XR_RBC_VALUE_BINDING_NEW || access->operation == XR_RBC_VALUE_BINDING_READ)) {
            role->region = region;
            role->fixed = true;
            return true;
        }
    }
    const XrLirArrayRegionBindings *bindings=c->value_code->array_regions;
    if (!bindings) return true;
    if (access->operation==XR_RBC_VALUE_EDGE) {
        if (access->index==UINT32_MAX) return true;
        if (access->index>=bindings->edge_count)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"selected edge lost its common region index");
        role->region=bindings->edges[access->index].region;
        if (role->region && bindings->regions[role->region-1].region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET)
            role->region=0; /* This consumer deliberately keeps every original read-only operation and move. */
        return true;
    }
    uint32_t block=c->diagnostic->block,instruction=c->diagnostic->instruction;
    if (block>=bindings->block_index_count || !bindings->block_index[block].region) return true;
    const XrLirArrayRegionBlock *row=&bindings->block_index[block];
    if (bindings->regions[row->region-1].region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET) return true;
    if (!instruction || instruction>row->instruction_count || row->first_operation>bindings->operation_count ||
        instruction>bindings->operation_count-row->first_operation)
        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"typed operation lost its common region position");
    role->region=row->region;
    role->operation=bindings->operations[row->first_operation+instruction-1];
    if (!role->operation.access && !role->operation.move && !role->operation.drop)
        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"typed operation lost its common region role");
    return true;
}
static bool rbc_lir_array_region_append(RbcLirCompiler *c,const XrRbcValueAccess *access) {
    XrRbcValueCode *code=c->value_code;
    if (!code->array_region_runtime) return true;
    XrRbcArrayRegionAccess role={0};
    if (!rbc_lir_array_region_role(c,access,&role)) return false;
    void *storage=(void *)code->array_region_accesses;
    if (!rbc_lir_grow(c,&storage,&c->value_region_access_capacity,code->access_count+1,sizeof(role))) return false;
    code->array_region_accesses=storage;
    ((XrRbcArrayRegionAccess *)storage)[code->access_count]=role;
    return true;
}
