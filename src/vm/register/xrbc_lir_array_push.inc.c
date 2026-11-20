/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Project sealed Array capacity windows onto physical bytecode limits.
 */
static bool rbc_lir_array_push_scalar(XrRbcValueLocation location) {
    return location.kind==XR_RBC_VALUE_REGISTER || location.kind==XR_RBC_VALUE_SPILL;
}

static bool rbc_lir_array_push_prepare(RbcLirCompiler *c) {
    if (!c->homes || !c->homes->array_push_window_count) return true;
    XrRbcValueCode *code=c->value_code;
    XrLirValueLayoutStatus status=xr_lir_array_push_window_bindings_create(c->homes,c->literals->layouts,
        &code->array_push_windows);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) {
        XrRbcCompileStatus compile_status=status==XR_LIR_VALUE_LAYOUT_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY :
            status==XR_LIR_VALUE_LAYOUT_OVERFLOW ? XR_RBC_COMPILE_LIMIT : XR_RBC_COMPILE_INVALID;
        return rbc_lir_fail(c,compile_status,"cannot project common Array capacity coordinates");
    }
    const XrLirArrayRegionBindings *bindings=code->array_push_windows;
    XrRbcArrayPushLimit *limits=NULL;
    for (size_t n=0;n<bindings->region_count;++n) {
        const XrLirArrayRegionBinding *row=&bindings->regions[n];
        const XrLirArrayPushCapacity *capacity=&row->region.push_capacity;
        if (!row->slot_count || !rbc_lir_array_push_scalar(code->locations[capacity->iv])) continue;
        XrLirInstructionView end={0};
        if (capacity->end_instruction &&
            (xr_lir_instruction_view(c->input,(XrLirBlock){c->input,c->function.id,capacity->end_block},
                capacity->end_instruction-1,&end)!=XR_LIR_OK || end.result!=capacity->end))
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Array capacity limit lost its SSA definition");
        bool constant=end.op==XR_LIR_CONST && end.type==XR_LIR_I64;
        if (!constant && ((capacity->end_block==row->region.header && capacity->end_instruction) ||
            !rbc_lir_array_push_scalar(code->locations[capacity->end]))) continue;
        if (!limits) {
            limits=xr_calloc(bindings->region_count,sizeof(*limits));
            if (!limits) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own Array capacity limits");
            code->array_push_limits=limits;
        }
        /* An immediate can lack storage, and a header constant's register
         * can be reused before the latch. Both use the verified SSA bits. */
        limits[n]=(XrRbcArrayPushLimit){.available=true,.value=constant ? 0 : capacity->end,
            .bits=constant ? end.bits : 0};
    }
    return true;
}

static uint32_t rbc_lir_array_push_edge(const XrRbcValueCode *code,uint32_t index) {
    if (!code->array_push_limits || index>=code->array_push_windows->edge_count) return 0;
    XrLirArrayRegionEdge edge=code->array_push_windows->edges[index];
    return (edge.kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY || edge.kind==XR_LIR_ARRAY_REGION_EDGE_LATCH) &&
        code->array_push_limits[edge.region-1].available ? edge.region : 0;
}
