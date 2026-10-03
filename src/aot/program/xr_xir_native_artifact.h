/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_native_artifact.h - Native bytes owned by one compiler ledger
 */
#ifndef XR_XIR_NATIVE_ARTIFACT_H
#define XR_XIR_NATIVE_ARTIFACT_H
#include "../../toolchain/xr_toolchain_binding.h"
#include "../../xir/xxir_checked.h"
#include "../../xir/xxir_program.h"

#define XR_XIR_NATIVE_INPUT_SCHEMA 2u
#define XR_XIR_NATIVE_ARTIFACT_SCHEMA 2u
typedef struct XrXirNativeInput {
    uint32_t schema_version, checked_schema, checked_contract;
    uint32_t value_abi, call_abi, program_abi, architecture;
    uint32_t entry, function_count, module_count;
    XrFingerprint source_checked_id, closed_checked_id, lowered_layout_id;
    XrFingerprint codegen_policy_id, generated_digest;
    XrToolchainBinding toolchain;
    XrFingerprint id;
} XrXirNativeInput;
typedef struct XrXirNativeArtifact XrXirNativeArtifact;
typedef struct XrXirNativeArtifactView {
    uint32_t schema_version, reserved;
    const uint8_t *bytes;
    size_t size;
    XrXirNativeInput input;
    XrFingerprint native_digest, id;
} XrXirNativeArtifactView;

/* Only independently validated product/SDK/target facts may supply admission.
 * This seal binds identities, not language authority. Context is mandatory;
 * input seal is allocation-free. Output bytes are preserved on every failure.
 * The artifact copies its context and bytes into one ledger allocation, which
 * outlives the producer's reference. byte_limit bounds native bytes only.
 * Work follows the toolchain identity primitives, plus actual owner allocation,
 * metadata/byte copies and clearing. Free consumes no work. */
XR_FUNC XrXirStatus xr_compile_native_input_seal(const XrXirCompileContext *context,
    const XrXirNativeInput *input, XrXirNativeInput *output);
XR_FUNC XrXirStatus xr_compile_native_artifact_seal(const XrXirCompileContext *context,
    const XrXirNativeInput *input, const void *bytes, size_t size,
    size_t byte_limit, XrXirNativeArtifact **output);
/* Verify charges the actual owner's retained ledger, never a caller replacement. */
XR_FUNC XrXirStatus xr_compile_native_artifact_verify(const XrXirNativeArtifact *artifact,
    const XrXirNativeInput *expected, size_t byte_limit);
/* Read-only facts borrow the owner lifetime; querying does not validate them. */
XR_FUNC const XrXirNativeArtifactView *xr_compile_native_artifact_view(const XrXirNativeArtifact *artifact);
XR_FUNC void xr_compile_native_artifact_free(XrXirNativeArtifact *artifact);
#endif // XR_XIR_NATIVE_ARTIFACT_H
