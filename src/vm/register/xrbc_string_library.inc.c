/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_library.inc.c - Synchronous access to actual String bindings
 */
static bool xr_rbc_string_reference(XrRbcStack *stack, const XrRbcModule *module,
    uint64_t bits, uint32_t source, XrtStringRef *output) {
    if (!bits) return false;
    if (bits & 1u) {
        XrRbcRegister *cell = xr_rbc_cell(stack, module, bits >> 1, XR_RBC_STRING);
        if (!cell) return false;
        *output = xrt_string_ref_local(&cell->string, source);
    } else *output = xrt_string_ref_state(*(XrtModuleAccess *)(uintptr_t)bits, source);
    return true;
}
static XrRbcStatus xr_rbc_string_library(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFrame *frame, const XrRbcFunction *f, uint32_t index, uint32_t source) {
    XrRbcAggregateAccess access = f->aggregate_accesses[index];
    const XrRbcArrayLocation *locations = f->aggregate_operands + access.first;
    XrRbcRegister *first = xr_rbc_aggregate_scalar(stack, frame, locations[0]);
    const XrtString *suffix = access.count == 2 ? xr_rbc_aggregate_scalar(stack, frame, locations[1])->string : NULL;
    if (access.slot == XR_LIR_STRING_OP_BORROW_CELL || access.slot == XR_LIR_STRING_OP_BORROW_STATE) {
        uint64_t bits;
        bool valid = access.slot == XR_LIR_STRING_OP_BORROW_CELL ?
            xr_rbc_array_ref_encode_cell(first->bits, &bits) : xr_rbc_array_ref_encode_state(first->state_ref, &bits);
        if (!valid) return XR_RBC_INVALID;
        xr_rbc_aggregate_scalar(stack, frame, access.result)->bits = bits;
    } else if (access.slot >= XR_LIR_STRING_OP_READ_REF) {
        XrtStringRef reference = {0};
        if (!xr_rbc_string_reference(stack, module, first->bits, source, &reference)) return XR_RBC_INVALID;
        if (access.slot == XR_LIR_STRING_OP_READ_REF)
            xr_rbc_aggregate_scalar(stack, frame, access.result)->string = xrt_string_ref_read(reference, source);
        else if (access.slot == XR_LIR_STRING_OP_WRITE_REF) xrt_string_ref_write(reference, suffix, source);
        else xrt_string_ref_append(reference, suffix, source);
    } else if (access.slot == XR_LIR_STRING_OP_APPEND_CELL) {
        XrRbcRegister *cell = xr_rbc_cell(stack, module, first->bits, XR_RBC_STRING);
        if (!cell) return XR_RBC_INVALID;
        xrt_string_cell_append(&cell->string, suffix, source);
    } else if (access.slot == XR_LIR_STRING_OP_APPEND_STATE) {
        xrt_string_ref_append(xrt_string_ref_state(*first->state_ref, source), suffix, source);
    } else {
        XrtString *result = xrt_string_append_owned(first->string, suffix, source);
        first->string = NULL;
        xr_rbc_aggregate_scalar(stack, frame, access.result)->string = result;
    }
    return XR_RBC_OK;
}
