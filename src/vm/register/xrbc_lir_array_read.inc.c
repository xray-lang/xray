/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Project the verified read reduction onto real scalar locations.
 */
static bool rbc_lir_array_read_slot(const XrRbcValueCode *code, uint32_t value) {
    if (!value || value > code->value_count) return false;
    XrRbcValueLocation location = code->locations[value];
    return location.kind == XR_RBC_VALUE_REGISTER || location.kind == XR_RBC_VALUE_SPILL;
}

static bool rbc_lir_array_read_ready(const XrRbcValueCode *code, const XrLirArraySetRegion *region) {
    const XrLirArrayReadSum *sum = &region->read_sum;
    return rbc_lir_array_read_slot(code, region->range.iv) &&
        rbc_lir_array_read_slot(code, sum->accumulator) &&
        (!sum->limit || rbc_lir_array_read_slot(code, sum->limit));
}

static uint32_t rbc_lir_array_read_edge(const XrRbcValueCode *code, uint32_t index) {
    const XrLirArrayRegionBindings *bindings = code->array_regions;
    if (!bindings || index >= bindings->edge_count) return 0;
    XrLirArrayRegionEdge edge = bindings->edges[index];
    if ((edge.kind != XR_LIR_ARRAY_REGION_EDGE_ENTRY && edge.kind != XR_LIR_ARRAY_REGION_EDGE_LATCH) ||
        !edge.region || (edge.kind == XR_LIR_ARRAY_REGION_EDGE_LATCH && !code->homes->edges[index].direct_header_move))
        return 0;
    const XrLirArrayRegionBinding *row = &bindings->regions[edge.region - 1];
    return row->slot_count && rbc_lir_array_read_ready(code, &row->region) ? edge.region : 0;
}
