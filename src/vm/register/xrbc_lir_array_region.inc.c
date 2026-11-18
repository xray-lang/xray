/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_array_region.inc.c - Stable typed access identities for common regions
 */
static bool rbc_lir_array_region_prepare(RbcLirCompiler *c) {
    if (!c->homes || !c->homes->array_region_count) return true;
    XrLirValueLayoutStatus status=xr_lir_array_region_bindings_create(c->homes,c->literals->layouts,
        &c->value_code->array_regions);
    if (status==XR_LIR_VALUE_LAYOUT_OK) {
        for (size_t n=0;n<c->value_code->array_regions->region_count;++n) {
            const XrLirArrayRegionBinding *row=&c->value_code->array_regions->regions[n];
            c->value_code->array_region_runtime|=row->slot_count && row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET;
        }
        return true;
    }
    XrRbcCompileStatus compile_failure=status==XR_LIR_VALUE_LAYOUT_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY :
        status==XR_LIR_VALUE_LAYOUT_OVERFLOW ? XR_RBC_COMPILE_LIMIT : XR_RBC_COMPILE_INVALID;
    return rbc_lir_fail(c,compile_failure,"cannot project common Array region coordinates");
}
static bool rbc_lir_array_region_role(RbcLirCompiler *c,const XrRbcValueAccess *access,XrRbcArrayRegionAccess *role) {
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
