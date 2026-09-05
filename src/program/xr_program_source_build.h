/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_source_build.h - App-neutral source-to-XrProgram build owner
 *
 * KEY CONCEPT:
 *   One owner keeps the module graph, analyzer evidence, and Xi graph alive
 *   until a canonical artifact and its retained validated view are complete.
 */

#ifndef XR_PROGRAM_SOURCE_BUILD_H
#define XR_PROGRAM_SOURCE_BUILD_H

#include "xr_program.h"
#include "xr_program_verify.h"

struct XrCompilerSession;
struct XrModuleIdentityAuthority;
struct XrModuleResolver;

#define XR_PROGRAM_SOURCE_BUILD_SCHEMA_VERSION UINT32_C(1)
#define XR_PROGRAM_SOURCE_BUILD_DEFAULT_MAX_MODULES UINT32_C(1024)
#define XR_PROGRAM_SOURCE_DIAGNOSTIC_MESSAGE_SIZE 512u

typedef enum XrProgramSourceProfile {
    XR_PROGRAM_SOURCE_PROFILE_INVALID = 0,
    XR_PROGRAM_SOURCE_PROFILE_CHECK,
    XR_PROGRAM_SOURCE_PROFILE_DEVELOPMENT,
    XR_PROGRAM_SOURCE_PROFILE_NATIVE_RELEASE,
    XR_PROGRAM_SOURCE_PROFILE_FREESTANDING,
    XR_PROGRAM_SOURCE_PROFILE_DEBUG_TOOLING,
} XrProgramSourceProfile;

typedef enum XrProgramSourceEntryKind {
    XR_PROGRAM_SOURCE_ENTRY_INVALID = 0,
    XR_PROGRAM_SOURCE_ENTRY_FUNCTION,
} XrProgramSourceEntryKind;

/* A source entry is identified before compiler-local evidence IDs exist.
 * Canonical module identity, exact source bytes, and the source declaration
 * name select one function without consulting an insertion-order ID. */
typedef struct XrProgramSourceEntryIdentity {
    uint8_t kind;
    uint8_t reserved8[3];
    const char *module_identity;
    const char *function_name;
    XrFingerprint source_content_fingerprint;
} XrProgramSourceEntryIdentity;

typedef struct XrProgramSourceBuildInput {
    uint32_t schema_version;
    uint32_t max_modules;
    struct XrCompilerSession *session;
    struct XrModuleResolver *resolver;
    const char *entry_source_path;
    const struct XrModuleIdentityAuthority *entry_authority;
    XrProgramSourceEntryIdentity entry;
    uint8_t source_profile;
    uint8_t reserved8[7];
    XrFingerprint semantic_profile_fingerprint;
} XrProgramSourceBuildInput;

typedef enum XrProgramSourceBuildStage {
    XR_PROGRAM_SOURCE_STAGE_NONE = 0,
    XR_PROGRAM_SOURCE_STAGE_REQUEST,
    XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH,
    XR_PROGRAM_SOURCE_STAGE_ANALYSIS,
    XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION,
    XR_PROGRAM_SOURCE_STAGE_CANONICALIZATION,
    XR_PROGRAM_SOURCE_STAGE_GLOBAL_EVIDENCE,
    XR_PROGRAM_SOURCE_STAGE_XI_PIPELINE,
    XR_PROGRAM_SOURCE_STAGE_IMPORT_RESOLUTION,
    XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION,
    XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE,
    XR_PROGRAM_SOURCE_STAGE_PROGRAM_VALIDATE,
    XR_PROGRAM_SOURCE_STAGE_SESSION_COMMIT,
} XrProgramSourceBuildStage;

typedef enum XrProgramSourceBuildStatus {
    XR_PROGRAM_SOURCE_BUILD_OK = 0,
    XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT,
    XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
    XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
    XR_PROGRAM_SOURCE_BUILD_GRAPH_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_MONOMORPHIZATION_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_CANONICALIZATION_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_EVIDENCE_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_PIPELINE_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED,
    XR_PROGRAM_SOURCE_BUILD_SESSION_REJECTED,
} XrProgramSourceBuildStatus;

typedef struct XrProgramSourceDiagnostic {
    XrProgramSourceBuildStatus status;
    XrProgramSourceBuildStage stage;
    uint32_t module_index;
    uint32_t source_line;
    uint32_t underlying_status;
    XrProgramBuildStatus writer_status;
    XrProgramVerifyStatus verifier_status;
    char message[XR_PROGRAM_SOURCE_DIAGNOSTIC_MESSAGE_SIZE];
} XrProgramSourceDiagnostic;

typedef struct XrProgramSourceProduct {
    XrProgramArtifact artifact;
    XrValidatedProgram *program;
} XrProgramSourceProduct;

/* product_out must be zero-initialized or previously freed. On success it owns
 * both the artifact and one validated-program reference; neither borrows the
 * compiler session, source graph, or the other's byte storage. */
XR_FUNC XrProgramSourceBuildStatus xr_program_source_build(
    const XrProgramSourceBuildInput *input, XrProgramSourceProduct *product_out,
    XrProgramSourceDiagnostic *diagnostic_out);
XR_FUNC void xr_program_source_product_free(XrProgramSourceProduct *product);
XR_FUNC const char *xr_program_source_build_status_name(XrProgramSourceBuildStatus status);

#endif /* XR_PROGRAM_SOURCE_BUILD_H */
