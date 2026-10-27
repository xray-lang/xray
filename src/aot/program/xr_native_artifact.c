/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_artifact.c - Exact native manifests with retained compiler ownership
 *
 * KEY CONCEPT:
 *   Rehashed metadata cannot match an independently constructed expected input.
 */
#include "xr_xir_native_artifact.h"
#include "../../base/xsha256.h"
#include <string.h>

_Static_assert(offsetof(XrXirNativeInput, source_checked_id) == 10 * sizeof(uint32_t),
    "The input scalar header must contain ten consecutive uint32 values");

struct XrXirNativeArtifact {
    size_t byte_count;
    XrXirNativeArtifactView view;
};
static bool native_nonzero(const uint8_t bytes[32]) {
    uint8_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) bits |= bytes[i];
    return bits != 0;
}
static void native_integer(XrSHA256Context *hash, uint64_t value, unsigned count) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < count; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    xr_sha256_update(hash, bytes, count);
}
static XrXirStatus native_fields(const XrXirNativeInput *input) {
    uint32_t fields[10];
    memcpy(fields, input, sizeof(fields));
    if (fields[0] != XR_XIR_NATIVE_INPUT_SCHEMA || fields[1] != XR_XIR_CHECKED_SCHEMA ||
        fields[2] != XR_XIR_CHECKED_CONTRACT || fields[3] != XR_XIR_VALUE_ABI_VERSION ||
        fields[4] != XR_XIR_CALL_ABI_VERSION || fields[5] != XR_XIR_PROGRAM_ABI_VERSION ||
        fields[6] != XR_XIR_ARCH_X86_64 || !fields[8] || !fields[9] || fields[7] >= fields[8])
        return XR_XIR_BAD_STRUCTURE;
    if (!native_nonzero(input->source_checked_id.bytes) ||
        !native_nonzero(input->closed_checked_id.bytes) ||
        !native_nonzero(input->lowered_layout_id.bytes) ||
        !native_nonzero(input->codegen_policy_id.bytes) ||
        !native_nonzero(input->generated_digest.bytes)) return XR_XIR_BAD_STRUCTURE;
    return xr_compile_toolchain_binding_validate(&input->toolchain) == XR_TOOLCHAIN_BINDING_OK ?
        XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static void native_input_id(const XrXirNativeInput *input, XrFingerprint *out) {
    static const char domain[] = "xray:xir-native-input:v2";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    native_integer(&hash, input->schema_version, 4);
    native_integer(&hash, input->checked_schema, 4);
    native_integer(&hash, input->checked_contract, 4);
    native_integer(&hash, input->value_abi, 4);
    native_integer(&hash, input->call_abi, 4);
    native_integer(&hash, input->program_abi, 4);
    native_integer(&hash, input->architecture, 4);
    native_integer(&hash, input->entry, 4);
    native_integer(&hash, input->function_count, 4);
    native_integer(&hash, input->module_count, 4);
    xr_sha256_update(&hash, input->source_checked_id.bytes, 32);
    xr_sha256_update(&hash, input->closed_checked_id.bytes, 32);
    xr_sha256_update(&hash, input->lowered_layout_id.bytes, 32);
    xr_sha256_update(&hash, input->codegen_policy_id.bytes, 32);
    xr_sha256_update(&hash, input->generated_digest.bytes, 32);
    xr_sha256_update(&hash, input->toolchain.id.bytes, 32);
    xr_sha256_final(&hash, out->bytes);
}
static XrXirStatus native_valid(const XrXirNativeInput *input) {
    XrXirStatus status = native_fields(input);
    if (status != XR_XIR_OK) return status;
    XrFingerprint expected;
    native_input_id(input, &expected);
    return memcmp(expected.bytes, input->id.bytes, 32) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_compile_native_input_seal(const XrXirNativeInput *input, XrXirNativeInput *output) {
    if (!input || !output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = native_fields(input);
    if (status != XR_XIR_OK) return status;
    XrXirNativeInput result;
    memcpy(&result, input, sizeof(result));
    native_input_id(&result, &result.id);
    memcpy(output, &result, sizeof(result));
    return XR_XIR_OK;
}
static void native_artifact_id(const XrXirNativeArtifactView *view, XrFingerprint *out) {
    static const char domain[] = "xray:xir-native-artifact:v2";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    native_integer(&hash, view->schema_version, 4);
    native_integer(&hash, (uint64_t)view->size, 8);
    xr_sha256_update(&hash, view->input.id.bytes, 32);
    xr_sha256_update(&hash, view->native_digest.bytes, 32);
    xr_sha256_final(&hash, out->bytes);
}
XR_FUNC XrXirStatus xr_compile_native_artifact_seal(const XrXirCompileContext *context,
    const XrXirNativeInput *input, const void *bytes, size_t size,
    size_t byte_limit, XrXirNativeArtifact **output) {
    if (!context || !context->resources || !input || !bytes || !size || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (size > SIZE_MAX - sizeof(XrXirNativeArtifact)) return XR_XIR_OVERFLOW;
    if (size > byte_limit) return XR_XIR_MEMORY_LIMIT;
    XrXirStatus status = native_valid(input);
    if (status != XR_XIR_OK) return status;
    void *memory = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_alloc(context->resources,
        sizeof(XrXirNativeArtifact) + size, &memory);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        if (allocated == XR_COMPILE_RESOURCE_MEMORY_LIMIT) return XR_XIR_MEMORY_LIMIT;
        if (allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
        if (allocated == XR_COMPILE_RESOURCE_OVERFLOW) return XR_XIR_OVERFLOW;
        return XR_XIR_BAD_STRUCTURE;
    }
    XrXirNativeArtifact *artifact = memory;
    memset(artifact, 0, sizeof(*artifact));
    memcpy(&artifact->view.input, input, sizeof(*input));
    memcpy(artifact + 1, bytes, size);
    artifact->view.schema_version = XR_XIR_NATIVE_ARTIFACT_SCHEMA;
    artifact->view.bytes = (const uint8_t *)(artifact + 1);
    artifact->view.size = size;
    artifact->byte_count = size;
    xr_sha256(artifact->view.bytes, size, artifact->view.native_digest.bytes);
    native_artifact_id(&artifact->view, &artifact->view.id);
    *output = artifact;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_compile_native_artifact_verify(const XrXirNativeArtifact *artifact,
    const XrXirNativeInput *expected, size_t byte_limit) {
    if (!artifact || !expected) return XR_XIR_BAD_STRUCTURE;
    const XrXirNativeArtifactView *view = &artifact->view;
    struct { uint32_t schema, reserved; const uint8_t *bytes; size_t size; } header;
    memcpy(&header, view, sizeof(header));
    /* The opaque owner supplies both the byte extent and its independent size. */
    if (header.schema != XR_XIR_NATIVE_ARTIFACT_SCHEMA || header.reserved ||
        !header.size || header.bytes != (const uint8_t *)(artifact + 1) || header.size != artifact->byte_count)
        return XR_XIR_BAD_STRUCTURE;
    if (header.size > byte_limit) return XR_XIR_MEMORY_LIMIT;
    XrXirStatus status = native_valid(expected);
    if (status != XR_XIR_OK) return status;
    status = native_valid(&view->input);
    if (status != XR_XIR_OK) return status;
    XrFingerprint digest, id;
    if (memcmp(expected->id.bytes, view->input.id.bytes, 32)) return XR_XIR_BAD_STRUCTURE;
    xr_sha256(view->bytes, view->size, digest.bytes);
    if (memcmp(digest.bytes, view->native_digest.bytes, 32)) return XR_XIR_BAD_STRUCTURE;
    native_artifact_id(view, &id);
    return memcmp(id.bytes, view->id.bytes, 32) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
XR_FUNC const XrXirNativeArtifactView *xr_compile_native_artifact_view(const XrXirNativeArtifact *artifact) {
    return artifact ? &artifact->view : NULL;
}
XR_FUNC void xr_compile_native_artifact_free(XrXirNativeArtifact *artifact) {
    xr_compile_resources_free(artifact);
}
