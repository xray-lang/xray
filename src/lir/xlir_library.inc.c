/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_library.inc.c - One receiving catalog for closed runtime library roles
 */
static XrLirStatus lir_library_catalog(XrLirModule *m, uint32_t flags) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    enum { string_count = sizeof(lir_string_signatures) / sizeof(*lir_string_signatures), library_id_base = 512 };
    XrLirIntrinsic local[string_count + 2 + LIR_SLICE_CATALOG_COUNT] = {{0}};
    XrLirIntrinsic *entries = local;
    if (flags & LIR_LIBRARY_MAP) {
        entries = xr_malloc((sizeof(local) / sizeof(*local) + LIR_MAP_CATALOG_COUNT) * sizeof(*entries));
        if (!entries) return lir_fail(m, XR_LIR_OUT_OF_MEMORY);
    }
    uint32_t count = 0;
    XrLirType slice_parameters[LIR_SLICE_CATALOG_COUNT][4] = {{0}};
    if (flags & LIR_LIBRARY_SLICE) {
        uint32_t added = 0;
        for (uint32_t family = 0; family < 4; ++family) {
            XrLirType element = family < 2 ? XR_LIR_I64 : XR_LIR_U8, slice = 0;
            status = xr_lir_slice_type(m, element, &slice);
            if (status != XR_LIR_OK) { if (entries != local) xr_free(entries); return status; }
            for (uint32_t role = 1; role < XR_LIR_SLICE_OP_CLEANUP; ++role) {
                if ((family & 1u) && role != XR_LIR_SLICE_OP_NEW) continue;
                uint32_t encoded = family * LIR_SLICE_ROLE_STRIDE + role;
                XrLirSliceIntrinsic shape;
                bool decoded = lir_slice_decode(LIR_SLICE_OPERATION_BASE + encoded, &shape);
                XR_CHECK(decoded && added < LIR_SLICE_CATALOG_COUNT, "the closed readonly catalog has bounded exact roles");
                XrLirType result;
                uint32_t arity = lir_slice_signature(shape, slice, slice_parameters[added], &result);
                entries[count++] = (XrLirIntrinsic){LIR_SLICE_ID_BASE + encoded,
                    slice_parameters[added++], arity, result, true, LIR_SLICE_OPERATION_BASE + encoded};
            }
        }
    }
    if (flags & LIR_LIBRARY_DISPLAY)
        entries[count++] = (XrLirIntrinsic){library_id_base,
            NULL, 0, XR_LIR_STRING, true, XR_LIR_INTRINSIC_DISPLAY};
    for (uint32_t n = 0; (flags & LIR_LIBRARY_STRING) && n < string_count; ++n) {
        const LirStringSignature *shape = &lir_string_signatures[n];
        entries[count++] = (XrLirIntrinsic){library_id_base + 1 + n,
            shape->parameters, shape->count, shape->result, true, XR_LIR_STRING_OP_APPEND_OWNED + n};
    }
    static const XrLirType range_parameters[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    if (flags & LIR_LIBRARY_RANGE)
        entries[count++] = (XrLirIntrinsic){library_id_base + 1 + string_count,
            range_parameters, 3, XR_LIR_UNIT, true, XR_LIR_RANGE_OP_CHECK};
    if (flags & LIR_LIBRARY_MAP) {
        XrLirType nullable = XR_LIR_TYPE_INVALID;
        status = xr_lir_nullable_type(m, XR_LIR_I64, &nullable);
        if (status != XR_LIR_OK) { xr_free(entries); return status; }
        uint32_t first = library_id_base + 2 + string_count;
        for (uint32_t n = 0; n < LIR_MAP_CATALOG_COUNT; ++n) {
            const LirMapSignature *shape = &lir_map_signatures[n];
            entries[count++] = (XrLirIntrinsic){first + n, shape->parameters, shape->count,
                shape->operation == XR_LIR_MAP_OP_GET ? nullable : shape->result, true, (uint32_t)shape->operation};
        }
    }
    XrLirRegistry registry = {entries, count};
    status = flags & (LIR_LIBRARY_ARRAY | LIR_LIBRARY_SLICE) ? lir_array_catalog(m, &registry) :
        xr_lir_set_registry(m, &registry);
    if (entries != local) xr_free(entries);
    return status == XR_LIR_OK ? status : lir_fail(m, status);
}
