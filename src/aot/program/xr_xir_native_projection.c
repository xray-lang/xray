/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_native_projection.c - Rechecked source to owned native C projection
 */
#include "xr_xir_native_projection.h"
#include "../../base/xsha256.h"
#include <string.h>

static XrXirStatus projection_prefix(const char *prefix, uint32_t *length) {
    if (!prefix) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i <= 64; ++i) {
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
static void projection_policy(const char *prefix, uint32_t length, XrFingerprint *output) {
    static const uint8_t domain[] = "xray:xir-c11-codegen-policy:v1";
    const uint32_t words[] = {1, XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT,
        XR_XIR_VALUE_ABI_VERSION, XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION, length};
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i) {
        uint8_t bytes[4];
        for (unsigned j = 0; j < sizeof(bytes); ++j) bytes[j] = (uint8_t)(words[i] >> (8u * j));
        xr_sha256_update(&hash, bytes, sizeof(bytes));
    }
    xr_sha256_update(&hash, (const uint8_t *)prefix, length);
    xr_sha256_final(&hash, output->bytes);
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
    uint32_t length = 0;
    XrXirStatus status = projection_prefix(request->prefix, &length);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_compile_source_product_verify(request->product, request->code_limit, diagnostic);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(request->product);
    if (!facts) return XR_XIR_BAD_STRUCTURE;
    XrXirNativeProjection *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(context->resources, 1,
        sizeof(*owner), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        if (allocated == XR_COMPILE_RESOURCE_MEMORY_LIMIT) return XR_XIR_MEMORY_LIMIT;
        if (allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
        if (allocated == XR_COMPILE_RESOURCE_OVERFLOW) return XR_XIR_OVERFLOW;
        return XR_XIR_BAD_STRUCTURE;
    }
    owner->context = *context;
    owner->facts.source = *facts;
    memcpy(owner->prefix, request->prefix, (size_t)length + 1);
    status = xr_xir_compile_source_product_emit(request->product, owner->prefix, request->code_limit, &owner->owned_source);
    if (status != XR_XIR_OK) {
        xr_compile_native_projection_owner_free(owner);
        return status;
    }
    projection_policy(owner->prefix, length, &owner->facts.codegen_policy_id);
    xr_sha256((const uint8_t *)owner->owned_source.text, owner->owned_source.length, owner->facts.generated_digest.bytes);
    owner->facts.prefix = owner->prefix;
    owner->source = (XrXirNativeProjectionSource){owner->owned_source.text, owner->owned_source.length};
    *output = owner;
    return XR_XIR_OK;
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
    XrXirNativeInput input;
    memset(&input, 0, sizeof(input));
    const XrXirSourceProductFacts *facts = &owner->facts.source;
    input.schema_version = XR_XIR_NATIVE_INPUT_SCHEMA;
    input.checked_schema = XR_XIR_CHECKED_SCHEMA; input.checked_contract = XR_XIR_CHECKED_CONTRACT;
    input.value_abi = XR_XIR_VALUE_ABI_VERSION; input.call_abi = XR_XIR_CALL_ABI_VERSION;
    input.program_abi = XR_XIR_PROGRAM_ABI_VERSION; input.architecture = facts->target.architecture;
    input.entry = facts->entry; input.function_count = facts->function_count; input.module_count = facts->module_count;
    memcpy(input.source_checked_id.bytes, facts->source_digest, 32);
    memcpy(input.closed_checked_id.bytes, facts->closed_digest, 32);
    memcpy(input.lowered_layout_id.bytes, facts->lowered_layout_digest, 32);
    input.codegen_policy_id = owner->facts.codegen_policy_id;
    input.generated_digest = owner->facts.generated_digest;
    input.toolchain = *binding;
    return xr_compile_native_input_seal(&input, output);
}
XR_FUNC void xr_compile_native_projection_owner_free(XrXirNativeProjection *owner) {
    if (!owner) return;
    xr_xir_compile_c_source_free(&owner->owned_source);
    xr_compile_resources_free(owner);
}
