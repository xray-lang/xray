/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_view_execute.inc.c - Cold typed calls into the canonical activation owner
 */
static XR_NOINLINE XrRbcStatus xr_rbc_view_step(XrRbcStack *stack, XrRbcFrame *frame,
    const XrRbcFunction *function, uint32_t index, uint32_t source) {
    const XrRbcViewAccess *row = &function->view_accesses[index];
    XrRbcRegister *result = rbc_view_signatures[row->operation].produces ?
        xr_rbc_array_location(stack, frame, row->result) : NULL;
    if (row->operation == XR_RBC_VIEW_FRAME_ENTER) {
        result->bits = xrt_view_frame_enter((uint32_t)row->bits, (uint32_t)(row->bits >> 32), source).identity;
        return XR_RBC_OK;
    }
    XrRbcRegister values[6] = {{0}};
    for (uint32_t p = 0; p < row->count; ++p)
        values[p] = *xr_rbc_array_location(stack, frame, row->operands[p]);
    XrtViewFrame activation = {values[0].bits};
    XrtViewCell cell = {values[1].bits};
    XrtSliceI64 slice = {values[1].bits};
    switch (row->operation) {
    case XR_RBC_VIEW_FRAME_LEAVE: xrt_view_frame_leave(activation, source); break;
    case XR_RBC_VIEW_CELL_NEW:
        if (values[1].bits > UINT32_MAX) return XR_RBC_INVALID;
        result->bits = xrt_view_cell_new(activation, (uint32_t)values[1].bits, values[2].array, source).identity;
        break;
    case XR_RBC_VIEW_CELL_READ: result->array = xrt_view_cell_read(activation, cell, source); break;
    case XR_RBC_VIEW_CELL_WRITE: xrt_view_cell_write(activation, cell, values[2].array, source); break;
    case XR_RBC_VIEW_CELL_DROP: xrt_view_cell_drop(activation, cell, source); break;
    case XR_RBC_VIEW_CELL_SET:
        xrt_view_cell_i64_set(activation, cell, xr_rbc_signed(values[2].bits), xr_rbc_signed(values[3].bits), source);
        break;
    case XR_RBC_VIEW_CELL_PUSH:
        xrt_view_cell_i64_push(activation, cell, xr_rbc_signed(values[2].bits), source); break;
    case XR_RBC_VIEW_CELL_CONSTRUCT: {
        if (values[3].bits != row->bits || values[3].bits > UINT32_MAX || !stack->module_accesses)
            return XR_RBC_INVALID;
        XrtModuleRef *reference = xrt_view_cell_construct(activation, cell, values[2].state,
            (uint32_t)values[3].bits, source);
        XrtModuleAccess *access = &stack->module_accesses[(size_t)row->bits * 2];
        xrt_module_require(xrt_module_reference_cache(reference, access), source);
        result->state_ref = access;
        break;
    }
    case XR_RBC_VIEW_CELL_IMPORT:
        if (values[1].bits > UINT32_MAX) return XR_RBC_INVALID;
        result->bits = xrt_view_cell_import(activation, (uint32_t)values[1].bits,
            values[2].state_ref->reference, source).identity;
        break;
    case XR_RBC_VIEW_NEW: case XR_RBC_VIEW_REBORROW: {
        if (values[1].bits > UINT32_MAX || values[5].bits > 1) return XR_RBC_INVALID;
        XrtViewRange range = {xr_rbc_signed(values[3].bits), xr_rbc_signed(values[4].bits), values[5].bits != 0};
        if (row->operation == XR_RBC_VIEW_NEW)
            result->bits = xrt_slice_i64_new(activation, (uint32_t)values[1].bits,
                (XrtViewCell){values[2].bits}, range, source).identity;
        else result->bits = xrt_slice_i64_reborrow(activation, (uint32_t)values[1].bits,
            (XrtSliceI64){values[2].bits}, range, source).identity;
        break;
    }
    case XR_RBC_VIEW_BIND:
        if (values[1].bits > UINT32_MAX) return XR_RBC_INVALID;
        result->bits = xrt_slice_i64_bind(activation, (uint32_t)values[1].bits,
            (XrtSliceI64){values[2].bits}, source).identity;
        break;
    case XR_RBC_VIEW_LEN: result->bits = (uint64_t)xrt_slice_i64_length(activation, slice, source); break;
    case XR_RBC_VIEW_GET:
        result->bits = (uint64_t)xrt_slice_i64_get(activation, slice, xr_rbc_signed(values[2].bits), source); break;
    case XR_RBC_VIEW_COPY: result->array = xrt_slice_i64_copy(activation, slice, source); break;
    case XR_RBC_VIEW_END: xrt_slice_i64_end(activation, slice, source); break;
    case XR_RBC_VIEW_RETURN: result->bits = xrt_slice_i64_return(activation, slice, source).identity; break;
    case XR_RBC_VIEW_CLEANUP:
        if (values[1].bits > UINT32_MAX) return XR_RBC_INVALID;
        xrt_slice_i64_cleanup(activation, (uint32_t)values[1].bits, source); break;
    default: return XR_RBC_INVALID;
    }
    return XR_RBC_OK;
}
