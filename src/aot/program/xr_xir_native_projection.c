/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_native_projection.c - Rechecked source to owned native C projection
 */
#include "xr_xir_native_projection.h"
#include "../../toolchain/xr_identity_work.h"

static XrXirStatus projection_prefix(XrIdentityWork *work, const char *prefix, uint32_t *length) {
    if (!prefix) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i <= 64; ++i) {
        if (!identity_work(work, 1)) return XR_XIR_BUDGET;
        unsigned char c = (unsigned char)prefix[i];
        if (!c) {
            if (!i) return XR_XIR_BAD_STRUCTURE;
            *length = i; return XR_XIR_OK;
        }
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9')) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_BAD_STRUCTURE;
}
static bool projection_policy(XrIdentityWork *work, const char *prefix, uint32_t length,
    XrFingerprint *output) {
    static const uint8_t domain[] = "xray:xir-c11-codegen-policy:v1";
    const uint32_t words[] = {1, XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT,
        XR_XIR_VALUE_ABI_VERSION, XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION, length};
    XrSHA256Context hash;
    if (!identity_begin(work, &hash) || !identity_bytes(work, &hash, domain, sizeof(domain) - 1)) return false;
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i)
        if (!identity_integer(work, &hash, words[i], 4)) return false;
    return identity_bytes(work, &hash, prefix, length) && identity_end(work, &hash, output->bytes);
}
XR_FUNC XrXirStatus xr_compile_native_projection_build(const XrXirNativeProjectionRequest *request,
    XrXirNativeProjection *output, XrXirDiagnostic *diagnostic) {
    if (!request || !output || output->source.text || output->source.length ||
        output->input.schema_version || !request->product || !request->toolchain || !request->code_limit)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context = xr_xir_compile_source_product_context(request->product);
    if (!context || !context->resources) return XR_XIR_BAD_STRUCTURE;
    XrIdentityWork work = {context->resources, false};
    for (unsigned i = 0; i < sizeof(output->input.id.bytes); ++i) {
        if (!identity_work(&work, 1)) return XR_XIR_BUDGET;
        if (output->input.id.bytes[i]) return XR_XIR_BAD_STRUCTURE;
    }
    XrToolchainBindingStatus binding = xr_compile_toolchain_binding_validate(context->resources, request->toolchain);
    if (binding != XR_TOOLCHAIN_BINDING_OK)
        return binding == XR_TOOLCHAIN_BINDING_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    uint32_t length = 0;
    XrXirStatus status = projection_prefix(&work, request->prefix, &length);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_compile_source_product_verify(request->product, request->code_limit, diagnostic);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(request->product);
    if (!facts) return XR_XIR_BAD_STRUCTURE;
    XrXirNativeProjection result;
    if (!identity_zero(&work, &result, sizeof(result))) return XR_XIR_BUDGET;
    status = xr_xir_compile_source_product_emit(request->product, request->prefix, request->code_limit, &result.source);
    if (status != XR_XIR_OK) return status;
    if (!identity_work(&work, 10 * sizeof(uint32_t))) {status = XR_XIR_BUDGET; goto done;}
    result.input.schema_version = XR_XIR_NATIVE_INPUT_SCHEMA;
    result.input.checked_schema = XR_XIR_CHECKED_SCHEMA; result.input.checked_contract = XR_XIR_CHECKED_CONTRACT;
    result.input.value_abi = XR_XIR_VALUE_ABI_VERSION; result.input.call_abi = XR_XIR_CALL_ABI_VERSION;
    result.input.program_abi = XR_XIR_PROGRAM_ABI_VERSION; result.input.architecture = facts->target.architecture;
    result.input.entry = facts->entry; result.input.function_count = facts->function_count;
    result.input.module_count = facts->module_count;
    XrSHA256Context hash;
    if (!identity_copy(&work, result.input.source_checked_id.bytes, facts->source_digest, 32) ||
        !identity_copy(&work, result.input.closed_checked_id.bytes, facts->closed_digest, 32) ||
        !identity_copy(&work, result.input.lowered_layout_id.bytes, facts->lowered_layout_digest, 32) ||
        !projection_policy(&work, request->prefix, length, &result.input.codegen_policy_id) ||
        !identity_begin(&work, &hash) || !identity_bytes(&work, &hash, result.source.text, result.source.length) ||
        !identity_end(&work, &hash, result.input.generated_digest.bytes) ||
        !identity_copy(&work, &result.input.toolchain, request->toolchain, sizeof(result.input.toolchain))) {
        status = XR_XIR_BUDGET; goto done;
    }
    status = xr_compile_native_input_seal(context, &result.input, &result.input);
    if (status == XR_XIR_OK && !identity_copy(&work, output, &result, sizeof(result))) status = XR_XIR_BUDGET;
done:
    if (status != XR_XIR_OK) xr_xir_compile_c_source_free(&result.source);
    return status;
}
XR_FUNC void xr_compile_native_projection_free(XrXirNativeProjection *projection) {
    if (!projection) return;
    xr_xir_compile_c_source_free(&projection->source);
    memset(projection, 0, sizeof(*projection));
}
