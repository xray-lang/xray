/* Exact schema1 public records/prototypes retained solely for old-object rejection.
 * Origin: 8a513e59d0163594a3ccd75dafbd63a5d0338c76, xr_backend_ir.h.
 * This test input is never linked into a product or used as a reader. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define XR_FUNC extern
typedef struct XrFingerprint { uint8_t bytes[32]; } XrFingerprint;
typedef XrFingerprint XrExecutionId;
typedef XrFingerprint XrBackendId;
typedef XrFingerprint XrOptimizationPolicyId;
typedef XrFingerprint XrToolchainId;
typedef XrFingerprint XrNativeArtifactId;
typedef enum XrBackendStatus {
    XR_BACKEND_OK = 0,
    XR_BACKEND_INVALID_INPUT,
    XR_BACKEND_UNSUPPORTED_OPERATION,
    XR_BACKEND_RESOURCE_LIMIT,
    XR_BACKEND_OUT_OF_MEMORY,
    XR_BACKEND_INVARIANT_REJECTED,
    XR_BACKEND_BINDING_REJECTED,
    XR_BACKEND_EMISSION_REJECTED,
    XR_BACKEND_TOOLCHAIN_REJECTED,
    XR_BACKEND_ARTIFACT_REJECTED,
} XrBackendStatus;

typedef struct XrGeneratedC {
    char *bytes;
    size_t size;
    char *header_bytes;
    size_t header_size;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint target_profile_id;
    XrFingerprint source_digest;
} XrGeneratedC;

typedef enum XrAotToolchainProvider {
    XR_AOT_TOOLCHAIN_INVALID = 0,
    XR_AOT_TOOLCHAIN_CLANG = 1,
    XR_AOT_TOOLCHAIN_GCC = 2,
    XR_AOT_TOOLCHAIN_MSVC = 3,
    XR_AOT_TOOLCHAIN_ZIG = 4,
} XrAotToolchainProvider;

typedef struct XrAotToolchainInput {
    uint32_t schema_version;
    uint8_t provider;
    uint8_t reserved8[3];
    const char *provider_version;
    const char *target_triple;
    const char *codegen_options;
    XrFingerprint sysroot_id;
    XrFingerprint runtime_objects_id;
    XrFingerprint target_profile_id;
} XrAotToolchainInput;

typedef struct XrAotToolchainBinding {
    uint32_t schema_version;
    uint8_t provider;
    uint8_t reserved8[3];
    XrFingerprint provider_version_id;
    XrFingerprint target_triple_id;
    XrFingerprint codegen_options_id;
    XrFingerprint sysroot_id;
    XrFingerprint runtime_objects_id;
    XrFingerprint target_profile_id;
    XrToolchainId id;
} XrAotToolchainBinding;

typedef struct XrNativeArtifact {
    uint8_t *bytes;
    size_t size;
    uint32_t schema_version;
    uint32_t reserved32;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrToolchainId toolchain_id;
    XrAotToolchainBinding toolchain_binding;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint target_profile_id;
    XrFingerprint native_digest;
    XrNativeArtifactId id;
} XrNativeArtifact;

XR_FUNC bool xr_aot_toolchain_binding_build(const XrAotToolchainInput *input,
                                            XrAotToolchainBinding *binding_out);
XR_FUNC bool xr_aot_toolchain_binding_equal(const XrAotToolchainBinding *left,
                                            const XrAotToolchainBinding *right);
XR_FUNC XrBackendStatus xr_native_artifact_seal(const XrGeneratedC *generated,
                                                const XrAotToolchainBinding *toolchain,
                                                const uint8_t *native_bytes, size_t native_size,
                                                XrNativeArtifact *artifact_out);
XR_FUNC bool xr_native_artifact_verify(const XrNativeArtifact *artifact, XrExecutionId execution_id,
                                       XrBackendId backend_id,
                                       XrOptimizationPolicyId optimization_policy_id,
                                       const XrAotToolchainBinding *toolchain);
XR_FUNC void xr_native_artifact_free(XrNativeArtifact *artifact);
