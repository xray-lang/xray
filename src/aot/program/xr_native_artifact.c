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
#include "../../toolchain/xr_identity_work.h"

_Static_assert(offsetof(XrXirNativeInput, source_checked_id) == 10 * sizeof(uint32_t),
    "The input scalar header must contain ten consecutive uint32 values");

struct XrXirNativeArtifact {
    XrXirCompileContext context;
    size_t byte_count;
    XrXirNativeArtifactView view;
};
static XrXirStatus native_failure(const XrIdentityWork *work) {
    return work->failed ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus native_fields(XrIdentityWork *work, const XrXirNativeInput *input) {
    uint32_t fields[10];
    if (!identity_copy(work, fields, input, sizeof(fields))) return XR_XIR_BUDGET;
    if (fields[0] != XR_XIR_NATIVE_INPUT_SCHEMA || fields[1] != XR_XIR_CHECKED_SCHEMA ||
        fields[2] != XR_XIR_CHECKED_CONTRACT || fields[3] != XR_XIR_VALUE_ABI_VERSION ||
        fields[4] != XR_XIR_CALL_ABI_VERSION || fields[5] != XR_XIR_PROGRAM_ABI_VERSION ||
        fields[6] != XR_XIR_ARCH_X86_64 || !fields[8] || !fields[9] || fields[7] >= fields[8])
        return XR_XIR_BAD_STRUCTURE;
    if (!identity_nonzero(work, input->source_checked_id.bytes) ||
        !identity_nonzero(work, input->closed_checked_id.bytes) ||
        !identity_nonzero(work, input->lowered_layout_id.bytes) ||
        !identity_nonzero(work, input->codegen_policy_id.bytes) ||
        !identity_nonzero(work, input->generated_digest.bytes)) return native_failure(work);
    XrToolchainBindingStatus status = xr_compile_toolchain_binding_validate(work->resources, &input->toolchain);
    if (status == XR_TOOLCHAIN_BINDING_BUDGET) { work->failed = true; return XR_XIR_BUDGET; }
    return status == XR_TOOLCHAIN_BINDING_OK ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static bool native_input_id(XrIdentityWork *work, const XrXirNativeInput *input, XrFingerprint *out) {
    static const char domain[] = "xray:xir-native-input:v2";
    XrSHA256Context hash;
    return identity_begin(work, &hash) && identity_bytes(work, &hash, domain, sizeof(domain) - 1) &&
        identity_integer(work, &hash, input->schema_version, 4) &&
        identity_integer(work, &hash, input->checked_schema, 4) &&
        identity_integer(work, &hash, input->checked_contract, 4) &&
        identity_integer(work, &hash, input->value_abi, 4) &&
        identity_integer(work, &hash, input->call_abi, 4) &&
        identity_integer(work, &hash, input->program_abi, 4) &&
        identity_integer(work, &hash, input->architecture, 4) &&
        identity_integer(work, &hash, input->entry, 4) &&
        identity_integer(work, &hash, input->function_count, 4) &&
        identity_integer(work, &hash, input->module_count, 4) &&
        identity_bytes(work, &hash, input->source_checked_id.bytes, 32) &&
        identity_bytes(work, &hash, input->closed_checked_id.bytes, 32) &&
        identity_bytes(work, &hash, input->lowered_layout_id.bytes, 32) &&
        identity_bytes(work, &hash, input->codegen_policy_id.bytes, 32) &&
        identity_bytes(work, &hash, input->generated_digest.bytes, 32) &&
        identity_bytes(work, &hash, input->toolchain.id.bytes, 32) && identity_end(work, &hash, out->bytes);
}
static XrXirStatus native_valid(XrIdentityWork *work, const XrXirNativeInput *input) {
    XrXirStatus status = native_fields(work, input);
    if (status != XR_XIR_OK) return status;
    XrFingerprint expected;
    return native_input_id(work, input, &expected) && identity_same(work, expected.bytes, input->id.bytes)
        ? XR_XIR_OK : native_failure(work);
}
XR_FUNC XrXirStatus xr_compile_native_input_seal(const XrXirCompileContext *context,
    const XrXirNativeInput *input, XrXirNativeInput *output) {
    if (!context || !context->resources || !input || !output) return XR_XIR_BAD_STRUCTURE;
    XrIdentityWork work = {context->resources, false};
    XrXirStatus status = native_fields(&work, input);
    if (status != XR_XIR_OK) return status;
    XrXirNativeInput result;
    if (!identity_copy(&work, &result, input, sizeof(result)) || !native_input_id(&work, &result, &result.id) ||
        !identity_copy(&work, output, &result, sizeof(result))) return native_failure(&work);
    return XR_XIR_OK;
}
static bool native_artifact_id(XrIdentityWork *work, const XrXirNativeArtifactView *view, XrFingerprint *out) {
    static const char domain[] = "xray:xir-native-artifact:v2";
    XrSHA256Context hash;
    return identity_begin(work, &hash) && identity_bytes(work, &hash, domain, sizeof(domain) - 1) &&
        identity_integer(work, &hash, view->schema_version, 4) &&
        identity_integer(work, &hash, (uint64_t)view->size, 8) &&
        identity_bytes(work, &hash, view->input.id.bytes, 32) &&
        identity_bytes(work, &hash, view->native_digest.bytes, 32) && identity_end(work, &hash, out->bytes);
}
static bool native_digest(XrIdentityWork *work, const void *bytes, size_t size, XrFingerprint *out) {
    XrSHA256Context hash;
    return identity_begin(work, &hash) && identity_bytes(work, &hash, bytes, size) && identity_end(work, &hash, out->bytes);
}
XR_FUNC XrXirStatus xr_compile_native_artifact_seal(const XrXirCompileContext *context,
    const XrXirNativeInput *input, const void *bytes, size_t size,
    size_t byte_limit, XrXirNativeArtifact **output) {
    if (!context || !context->resources || !input || !bytes || !size || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (size > byte_limit || size > SIZE_MAX - sizeof(XrXirNativeArtifact)) return XR_XIR_BUDGET;
    XrIdentityWork work = {context->resources, false};
    XrXirStatus status = native_valid(&work, input);
    if (status != XR_XIR_OK) return status;
    void *memory = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_alloc(work.resources, sizeof(XrXirNativeArtifact) + size, &memory);
    if (allocated != XR_COMPILE_RESOURCE_OK)
        return allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrXirNativeArtifact *artifact = memory;
    if (!identity_zero(&work, artifact, sizeof(*artifact)) ||
        !identity_copy(&work, &artifact->context, context, sizeof(*context)) ||
        !identity_copy(&work, &artifact->view.input, input, sizeof(*input)) ||
        !identity_copy(&work, ((uint8_t *)(artifact + 1)), bytes, size) ||
        !identity_work(&work, sizeof(artifact->view.schema_version) + sizeof(artifact->view.bytes) +
            sizeof(artifact->view.size) + sizeof(artifact->byte_count))) goto failed;
    artifact->view.schema_version = XR_XIR_NATIVE_ARTIFACT_SCHEMA;
    artifact->view.bytes = ((uint8_t *)(artifact + 1));
    artifact->view.size = size;
    artifact->byte_count = size;
    if (!native_digest(&work, ((uint8_t *)(artifact + 1)), size, &artifact->view.native_digest) ||
        !native_artifact_id(&work, &artifact->view, &artifact->view.id) ||
        !identity_copy(&work, output, &artifact, sizeof(artifact))) goto failed;
    return XR_XIR_OK;
failed:
    xr_compile_resources_free(artifact);
    return native_failure(&work);
}
XR_FUNC XrXirStatus xr_compile_native_artifact_verify(const XrXirNativeArtifact *artifact,
    const XrXirNativeInput *expected, size_t byte_limit) {
    if (!artifact || !expected) return XR_XIR_BAD_STRUCTURE;
    XrIdentityWork work = {artifact->context.resources, false};
    const XrXirNativeArtifactView *view = &artifact->view;
    struct { uint32_t schema, reserved; const uint8_t *bytes; size_t size; } header;
    if (!identity_copy(&work, &header, view, sizeof(header))) return XR_XIR_BUDGET;
    /* The opaque owner, not a caller-provided record, supplies this context. */
    if (header.schema != XR_XIR_NATIVE_ARTIFACT_SCHEMA || header.reserved ||
        !header.size || header.bytes != ((uint8_t *)(artifact + 1)) || header.size != artifact->byte_count)
        return XR_XIR_BAD_STRUCTURE;
    if (header.size > byte_limit) return XR_XIR_BUDGET;
    XrXirStatus status = native_valid(&work, expected);
    if (status != XR_XIR_OK) return status;
    status = native_valid(&work, &view->input);
    if (status != XR_XIR_OK) return status;
    XrFingerprint digest, id;
    if (!identity_same(&work, expected->id.bytes, view->input.id.bytes) ||
        !native_digest(&work, view->bytes, view->size, &digest) ||
        !identity_same(&work, digest.bytes, view->native_digest.bytes) ||
        !native_artifact_id(&work, view, &id) || !identity_same(&work, id.bytes, view->id.bytes))
        return native_failure(&work);
    return XR_XIR_OK;
}
XR_FUNC const XrXirNativeArtifactView *xr_compile_native_artifact_view(const XrXirNativeArtifact *artifact) {
    return artifact ? &artifact->view : NULL;
}
XR_FUNC void xr_compile_native_artifact_free(XrXirNativeArtifact *artifact) {
    xr_compile_resources_free(artifact);
}
