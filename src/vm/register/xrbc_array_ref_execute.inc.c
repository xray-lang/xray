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
