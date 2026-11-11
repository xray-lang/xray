/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_array_ref_execute.inc.c - Stable tagged frame indices and module caches
 */
_Static_assert(sizeof(uintptr_t) <= sizeof(uint64_t) && _Alignof(XrtModuleAccess) >= 2,
    "Array references distinguish aligned module caches from indexed frame cells");
static XrtModuleTypeKind xr_rbc_array_module_kind(XrRbcRegisterKind kind) {
    static const XrtModuleTypeKind kinds[] = {XRT_MODULE_ARRAY_I64, XRT_MODULE_ARRAY_STRING,
        XRT_MODULE_ARRAY_U8, XRT_MODULE_ARRAY_U64};
    uint32_t family = xr_rbc_array_family(kind);
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XRT_MODULE_TYPE_INVALID;
}
static bool xr_rbc_array_ref_encode_cell(uint64_t handle, uint64_t *output) {
    if (!handle || handle > UINT64_MAX / 2) return false;
    *output = (handle << 1) | UINT64_C(1);
    return true;
}
static bool xr_rbc_array_ref_encode_state(XrtModuleAccess *cache, uint64_t *output) {
    uintptr_t address = (uintptr_t)cache;
    if (!address || (address & 1u)) return false;
    *output = (uint64_t)address;
    return true;
}
static bool xr_rbc_array_ref_decode(XrRbcStack *stack, const XrRbcModule *module, uint64_t bits,
    XrRbcRegisterKind kind, uint32_t source, XrtArrayRef *output) {
    XrtModuleTypeKind payload = xr_rbc_array_module_kind(kind);
    if (!bits || payload == XRT_MODULE_TYPE_INVALID) return false;
    if (bits & 1u) {
        /* A frame handle survives every stack growth. Only this operation
         * borrows the actual binding address, across no guest call. */
        XrRbcRegister *cell = xr_rbc_cell(stack, module, bits >> 1, kind);
        if (!cell) return false;
        *output = xrt_array_ref_local(&cell->array, payload, source);
    } else {
        XrtModuleAccess *cache = (XrtModuleAccess *)(uintptr_t)bits;
        if (cache->kind != payload || !cache->is_mutable) return false;
        *output = (XrtArrayRef){.state=*cache};
    }
    return true;
}
static XrRbcStatus xr_rbc_array_reference_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFrame *frame, const XrRbcFunction *function, XrRbcArrayAccess access, uint32_t source) {
    const XrRbcArrayLocation *locations = function->array_operands + access.first;
    XrRbcRegister *first = xr_rbc_array_location(stack, frame, locations[0]);
    XrRbcRegister *result = xr_rbc_array_location(stack, frame, access.result);
    if (access.operation == XR_RBC_ARRAY_BORROW) {
        XrRbcRegisterKind kind = function->register_kinds[locations[0].index];
        bool valid = kind == XR_RBC_STATE_REF ? xr_rbc_array_ref_encode_state(first->state_ref, &result->bits) :
            xr_rbc_array_ref_encode_cell(first->bits, &result->bits);
        return valid ? XR_RBC_OK : XR_RBC_INVALID;
    }
    XrtArrayRef reference = {0};
    if (!xr_rbc_array_ref_decode(stack, module, first->bits, access.kind, source, &reference))
        return XR_RBC_INVALID;
    if (access.operation == XR_RBC_ARRAY_REF_READ) {
        result->array = xrt_array_ref_read(reference, source);
        return XR_RBC_OK;
    }
    XrRbcRegister *second = xr_rbc_array_location(stack, frame, locations[1]);
    if (access.operation == XR_RBC_ARRAY_REF_WRITE) {
        xrt_array_ref_write(reference, second->array, source);
        return XR_RBC_OK;
    }
    bool set = access.operation == XR_RBC_ARRAY_REF_SET;
    XrRbcRegister *value = xr_rbc_array_location(stack, frame, locations[set ? 2 : 1]);
    if (access.kind == XR_RBC_ARRAY_U8 && value->bits > UINT8_MAX) return XR_RBC_INVALID;
    int64_t offset = set ? xr_rbc_signed(second->bits) : 0;
    if (set) {
        switch (access.kind) {
        case XR_RBC_ARRAY_I64: xrt_array_ref_i64_set(reference, offset, xr_rbc_signed(value->bits), source); break;
        case XR_RBC_ARRAY_STRING: xrt_array_ref_string_set(reference, offset, value->string, source); break;
        case XR_RBC_ARRAY_U8: xrt_array_ref_u8_set(reference, offset, (uint8_t)value->bits, source); break;
        case XR_RBC_ARRAY_U64: xrt_array_ref_u64_set(reference, offset, value->bits, source); break;
        default: return XR_RBC_INVALID;
        }
    } else {
        switch (access.kind) {
        case XR_RBC_ARRAY_I64: xrt_array_ref_i64_push(reference, xr_rbc_signed(value->bits), source); break;
        case XR_RBC_ARRAY_STRING: xrt_array_ref_string_push(reference, value->string, source); break;
        case XR_RBC_ARRAY_U8: xrt_array_ref_u8_push(reference, (uint8_t)value->bits, source); break;
        case XR_RBC_ARRAY_U64: xrt_array_ref_u64_push(reference, value->bits, source); break;
        default: return XR_RBC_INVALID;
        }
    }
    return XR_RBC_OK;
}
