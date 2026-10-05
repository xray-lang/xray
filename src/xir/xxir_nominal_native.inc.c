/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_native.inc.c - Owned governed value-enum metadata admission
 *
 * KEY CONCEPT:
 *   A native record never replaces declaration shape or module authority.
 */
static bool nominal_native_charge(void *owner, uint64_t work) {
    return xir_compile_work((const XrXirCompileContext *)owner, work);
}
static XrXirStatus nominal_native_name(const XrXirCompileContext *context,
    XrXirLiteral actual, const char *expected) {
    size_t length = 0;
    for (;;) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (!expected[length]) break;
        ++length;
    }
    if (!xir_compile_work(context, actual.length == length ? length : 1)) return XR_XIR_BUDGET;
    return actual.length == length && !memcmp(actual.bytes, expected, length) ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus nominal_native_verify(const XrXirCompileContext *context,
    const XrXirNominalIdentity *identity) {
    const XrXirNominalNativeRecord *record = &identity->native;
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    if (!record->native_id) {
        for (unsigned i = 0; i < sizeof(record->source_fingerprint); ++i) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            if (record->source_fingerprint[i]) return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    if (record->native_id != XR_NATIVE_DECLARATION_ORDERING || identity->kind != XR_XIR_NOMINAL_ENUM ||
        identity->exported != 1 || identity->arity || identity->field_count || identity->fields ||
        identity->flags || identity->variant_count != 5 || !identity->variants) return XR_XIR_BAD_STRUCTURE;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(record->native_id);
    XrNativeDeclarationWork work = {(void *)context, nominal_native_charge};
    XrNativeDeclarationStatus admitted = xr_native_declaration_admit(&work, native);
    if (admitted != XR_NATIVE_DECLARATION_OK)
        return admitted == XR_NATIVE_DECLARATION_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    if (!native || native->kind != XR_NATIVE_DECLARATION_VALUE || native->member_count != 5)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, sizeof(record->source_fingerprint))) return XR_XIR_BUDGET;
    if (memcmp(record->source_fingerprint, native->source_fingerprint.bytes, sizeof(record->source_fingerprint)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = nominal_native_name(context, identity->module,
        "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def");
    if (status == XR_XIR_OK) status = nominal_native_name(context, identity->name, native->name);
    for (uint32_t i = 0; i < 5 && status == XR_XIR_OK; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirNominalVariant *variant = &identity->variants[i];
        if (variant->field_begin || variant->field_count) return XR_XIR_BAD_STRUCTURE;
        status = nominal_native_name(context, variant->name, native->members[i].name);
    }
    return status;
}
