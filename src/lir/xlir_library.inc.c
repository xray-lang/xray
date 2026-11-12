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
    enum { string_count = sizeof(lir_string_signatures) / sizeof(*lir_string_signatures) };
    XrLirIntrinsic entries[XR_LIR_VIEW_OP_COUNT + string_count] = {{0}};
    uint32_t count = 0;
    for (uint32_t role = 1; (flags & LIR_LIBRARY_VIEW) && role < XR_LIR_VIEW_OP_COUNT; ++role) {
        const LirViewSignature *shape = &lir_view_signatures[role];
        entries[count++] = (XrLirIntrinsic){XR_LIR_ARRAY_CATALOG_COUNT + role,
            shape->parameters, shape->count, shape->result, true, role};
    }
    if (flags & LIR_LIBRARY_DISPLAY)
        entries[count++] = (XrLirIntrinsic){XR_LIR_ARRAY_CATALOG_COUNT + XR_LIR_VIEW_OP_COUNT,
            NULL, 0, XR_LIR_STRING, true, XR_LIR_INTRINSIC_DISPLAY};
    for (uint32_t n = 0; (flags & LIR_LIBRARY_STRING) && n < string_count; ++n) {
        const LirStringSignature *shape = &lir_string_signatures[n];
        entries[count++] = (XrLirIntrinsic){XR_LIR_ARRAY_CATALOG_COUNT + XR_LIR_VIEW_OP_COUNT + 1 + n,
            shape->parameters, shape->count, shape->result, true, XR_LIR_STRING_OP_APPEND_OWNED + n};
    }
    XrLirRegistry registry = {entries, count};
    status = flags & (LIR_LIBRARY_ARRAY | LIR_LIBRARY_VIEW) ? lir_array_catalog(m, &registry) :
        xr_lir_set_registry(m, &registry);
    return status == XR_LIR_OK ? status : lir_fail(m, status);
}
