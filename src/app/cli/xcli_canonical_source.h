/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_canonical_source.h - Shared CLI source-to-program request adapter
 *
 * KEY CONCEPT:
 *   Every source product command derives the same exact module identity and
 *   source snapshot before delegating to the app-neutral program owner.
 */

#ifndef XCLI_CANONICAL_SOURCE_H
#define XCLI_CANONICAL_SOURCE_H

#include "../../program/xr_program_source_build.h"

struct XrVMRuntime;

#define XR_CLI_CANONICAL_SOURCE_SCHEMA_VERSION UINT32_C(2)
#define XR_CLI_CANONICAL_SOURCE_DIAGNOSTIC_SIZE 512u

typedef struct XrCliCanonicalSourceRequest {
    uint32_t schema_version;
    struct XrVMRuntime *compiler_host;
    const char *entry_source_path;
    const char *entry_function;
    uint8_t entry_kind;
    uint8_t source_profile;
    uint8_t reserved8[6];
    XrFingerprint semantic_profile_fingerprint;
} XrCliCanonicalSourceRequest;

typedef enum XrCliCanonicalSourceStatus {
    XR_CLI_CANONICAL_SOURCE_OK = 0,
    XR_CLI_CANONICAL_SOURCE_INVALID_INPUT,
    XR_CLI_CANONICAL_SOURCE_AUTHORITY_REJECTED,
    XR_CLI_CANONICAL_SOURCE_SOURCE_REJECTED,
    XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED,
} XrCliCanonicalSourceStatus;

typedef struct XrCliCanonicalSourceDiagnostic {
    XrCliCanonicalSourceStatus status;
    XrProgramSourceDiagnostic build;
    char message[XR_CLI_CANONICAL_SOURCE_DIAGNOSTIC_SIZE];
} XrCliCanonicalSourceDiagnostic;

XR_FUNC XrCliCanonicalSourceStatus xr_cli_canonical_source_build(
    const XrCliCanonicalSourceRequest *request, XrProgramSourceProduct *product_out,
    XrCliCanonicalSourceDiagnostic *diagnostic_out);
XR_FUNC const char *xr_cli_canonical_source_status_name(XrCliCanonicalSourceStatus status);

#endif  // XCLI_CANONICAL_SOURCE_H
