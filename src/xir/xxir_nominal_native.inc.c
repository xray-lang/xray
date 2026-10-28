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
static XrXirStatus nominal_native_name(
    XrXirLiteral actual, const char *expected) {
    size_t length = 0;
    for (;;) {

        if (!expected[length]) break;
        ++length;
    }

    return actual.length == length && !memcmp(actual.bytes, expected, length) ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus nominal_native_verify(
    const XrXirNominalIdentity *identity) {
    const XrXirNominalNativeRecord *record = &identity->native;

    if (!record->native_id) {
        for (unsigned i = 0; i < sizeof(record->source_fingerprint); ++i) {

            if (record->source_fingerprint[i]) return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    uint32_t arity=0, fields=0, variants=0;
    if (record->native_id==XR_NATIVE_DECLARATION_ORDERING) variants=5;
    else if (record->native_id==XR_NATIVE_DECLARATION_RECV) { arity=1; fields=1; variants=4; }
    else if (record->native_id==XR_NATIVE_DECLARATION_SEND_RESULT) variants=4;
    else if (record->native_id==XR_NATIVE_DECLARATION_UTF8_ERROR ||
        record->native_id==XR_NATIVE_DECLARATION_CANCELLED) variants=1;
    else return XR_XIR_BAD_STRUCTURE;
    if (identity->kind!=XR_XIR_NOMINAL_ENUM || identity->exported!=1 || identity->arity!=arity ||
        identity->field_count!=fields || (!fields && identity->fields) || identity->flags ||
        identity->variant_count!=variants || !identity->variants) return XR_XIR_BAD_STRUCTURE;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(record->native_id);
    if (!xr_native_declaration_validate(native)) return XR_XIR_BAD_STRUCTURE;
    if (!native || native->kind!=XR_NATIVE_DECLARATION_VALUE || native->member_count!=variants ||
        native->parameter_count!=arity) return XR_XIR_BAD_STRUCTURE;

    if (memcmp(record->source_fingerprint,native->source_fingerprint.bytes,sizeof(record->source_fingerprint)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=nominal_native_name(identity->module,
        "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def");
    if (status==XR_XIR_OK) status=nominal_native_name(identity->name,native->name);
    /* Raw declaration headers do not yet carry the field-name projection. */
    if (fields && identity->fields) {
        if (identity->fields[0].flags) return XR_XIR_BAD_STRUCTURE;
        if (status==XR_XIR_OK) status=nominal_native_name(identity->fields[0].name,"value");
    }
    uint32_t begin=0;
    for (uint32_t i=0;i<variants && status==XR_XIR_OK;++i) {

        const XrXirNominalVariant *variant=&identity->variants[i];
        uint32_t count=fields && !i ? 1u : 0u;
        if (variant->field_begin!=begin || variant->field_count!=count) return XR_XIR_BAD_STRUCTURE;
        begin+=count;
        status=nominal_native_name(variant->name,native->members[i].name);
    }
    return status;
}
