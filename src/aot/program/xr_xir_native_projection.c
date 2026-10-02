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
struct XrXirNativeProjection {
    XrXirCompileContext context;
    XrXirCSource owned_source;
    XrXirNativeProjectionSource source;
    XrXirNativeProjectionFacts facts;
    char prefix[65];
};
XR_FUNC XrXirStatus xr_compile_native_projection_prepare(const XrXirNativeProjectionRequest *request,
    XrXirNativeProjection **output, XrXirDiagnostic *diagnostic) {
    if (!request || !output || *output || !request->product || !request->code_limit)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context = xr_xir_compile_source_product_context(request->product);
    if (!context || !context->resources) return XR_XIR_BAD_STRUCTURE;
    XrIdentityWork work = {context->resources, false};
    uint32_t length = 0;
    XrXirStatus status = projection_prefix(&work, request->prefix, &length);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_compile_source_product_verify(request->product, request->code_limit, diagnostic);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(request->product);
    if (!facts) return XR_XIR_BAD_STRUCTURE;
    XrXirNativeProjection *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(context->resources, 1,
        sizeof(*owner), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK)
        return allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    if (!identity_copy(&work, &owner->context, context, sizeof(*context)) ||
        !identity_copy(&work, &owner->facts.source, facts, sizeof(*facts)) ||
        !identity_copy(&work, owner->prefix, request->prefix, (size_t)length + 1)) {
        status = XR_XIR_BUDGET; goto done;
    }
    status = xr_xir_compile_source_product_emit(request->product, owner->prefix, request->code_limit, &owner->owned_source);
    if (status != XR_XIR_OK) goto done;
    XrSHA256Context hash;
    if (!projection_policy(&work, owner->prefix, length, &owner->facts.codegen_policy_id) ||
        !identity_begin(&work, &hash) || !identity_bytes(&work, &hash, owner->owned_source.text, owner->owned_source.length) ||
        !identity_end(&work, &hash, owner->facts.generated_digest.bytes) ||
        !identity_work(&work, sizeof(owner->facts.prefix) + sizeof(owner->source))) {
        status = XR_XIR_BUDGET; goto done;
    }
    owner->facts.prefix = owner->prefix;
    owner->source = (XrXirNativeProjectionSource){owner->owned_source.text, owner->owned_source.length};
    if (!identity_copy(&work, output, &owner, sizeof(owner))) status = XR_XIR_BUDGET;
done:
    if (status != XR_XIR_OK) xr_compile_native_projection_owner_free(owner);
    return status;
}
XR_FUNC const XrXirCompileContext *xr_compile_native_projection_context(const XrXirNativeProjection *owner) {
    return owner ? &owner->context : NULL;
}
XR_FUNC const XrXirNativeProjectionSource *xr_compile_native_projection_source(const XrXirNativeProjection *owner) {
    return owner ? &owner->source : NULL;
}
XR_FUNC const XrXirNativeProjectionFacts *xr_compile_native_projection_facts(const XrXirNativeProjection *owner) {
    return owner ? &owner->facts : NULL;
}
XR_FUNC XrXirStatus xr_compile_native_projection_bind(const XrXirNativeProjection *owner,
    const XrToolchainBinding *binding, XrXirNativeInput *output) {
    if (!owner || !binding || !output) return XR_XIR_BAD_STRUCTURE;
    XrIdentityWork work = {owner->context.resources, false};
    XrXirNativeInput input;
    const XrXirSourceProductFacts *facts = &owner->facts.source;
    if (!identity_zero(&work, &input, sizeof(input)) ||
        !identity_work(&work, 10 * sizeof(uint32_t))) return XR_XIR_BUDGET;
    input.schema_version = XR_XIR_NATIVE_INPUT_SCHEMA;
    input.checked_schema = XR_XIR_CHECKED_SCHEMA; input.checked_contract = XR_XIR_CHECKED_CONTRACT;
    input.value_abi = XR_XIR_VALUE_ABI_VERSION; input.call_abi = XR_XIR_CALL_ABI_VERSION;
    input.program_abi = XR_XIR_PROGRAM_ABI_VERSION; input.architecture = facts->target.architecture;
    input.entry = facts->entry; input.function_count = facts->function_count; input.module_count = facts->module_count;
    if (!identity_copy(&work, input.source_checked_id.bytes, facts->source_digest, 32) ||
        !identity_copy(&work, input.closed_checked_id.bytes, facts->closed_digest, 32) ||
        !identity_copy(&work, input.lowered_layout_id.bytes, facts->lowered_layout_digest, 32) ||
        !identity_copy(&work, &input.codegen_policy_id, &owner->facts.codegen_policy_id, sizeof(XrFingerprint)) ||
        !identity_copy(&work, &input.generated_digest, &owner->facts.generated_digest, sizeof(XrFingerprint)) ||
        !identity_copy(&work, &input.toolchain, binding, sizeof(*binding))) return XR_XIR_BUDGET;
    return xr_compile_native_input_seal(&owner->context, &input, output);
}
XR_FUNC void xr_compile_native_projection_owner_free(XrXirNativeProjection *owner) {
    if (!owner) return;
    xr_xir_compile_c_source_free(&owner->owned_source);
    xr_compile_resources_free(owner);
}
