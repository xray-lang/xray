/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_canonical_source.c - Shared CLI source-to-program request adapter
 *
 * KEY CONCEPT:
 *   CLI commands do not assemble private compiler stages. They establish one
 *   exact graph authority and hand one immutable request to the source owner.
 */

#include "xcli_canonical_source.h"

#include "xcli_graph_authority.h"
#include "../../base/xchecks.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_identity.h"
#include "../../runtime/xisolate_api.h"
#include "../../toolchain/xcompiler_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void clear_diagnostic(XrCliCanonicalSourceDiagnostic *diagnostic) {
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
}

static XrCliCanonicalSourceStatus reject(XrCliCanonicalSourceDiagnostic *diagnostic,
                                         XrCliCanonicalSourceStatus status, const char *format,
                                         ...) {
    if (diagnostic) {
        diagnostic->status = status;
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, arguments);
        va_end(arguments);
    }
    return status;
}

static bool fingerprint_present(XrFingerprint fingerprint) {
    uint8_t combined = 0u;
    for (size_t index = 0u; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined != 0u;
}

static bool request_valid(const XrCliCanonicalSourceRequest *request) {
    bool function_entry = request && request->entry_kind == XR_PROGRAM_SOURCE_ENTRY_FUNCTION;
    bool initializer_entry =
        request && request->entry_kind == XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER;
    if (!request || request->schema_version != XR_CLI_CANONICAL_SOURCE_SCHEMA_VERSION ||
        !request->compiler_host || !request->entry_source_path ||
        request->entry_source_path[0] == '\0' || (!function_entry && !initializer_entry) ||
        (function_entry && (!request->entry_function || request->entry_function[0] == '\0')) ||
        (initializer_entry && request->entry_function != NULL) ||
        !fingerprint_present(request->semantic_profile_fingerprint))
        return false;
    for (size_t index = 0u; index < sizeof(request->reserved8); ++index)
        if (request->reserved8[index] != 0u)
            return false;
    return request->source_profile > XR_PROGRAM_SOURCE_PROFILE_INVALID &&
           request->source_profile <= XR_PROGRAM_SOURCE_PROFILE_DEBUG_TOOLING;
}

static bool read_source_fingerprint(const char *path, XrFingerprint *fingerprint, char *error,
                                    size_t error_size) {
    size_t size = 0u;
    char *source = xr_file_read_all(path, "r", &size);
    if (!source) {
        snprintf(error, error_size, "cannot open entry source");
        return false;
    }
    if ((uint64_t) size > XR_PROGRAM_LIMIT_ARTIFACT_BYTES) {
        xr_free(source);
        snprintf(error, error_size, "entry source exceeds the bounded source budget");
        return false;
    }
    xr_module_source_fingerprint(source, fingerprint);
    xr_free(source);
    return true;
}

XrCliCanonicalSourceStatus
xr_cli_canonical_source_build(const XrCliCanonicalSourceRequest *request,
                              XrProgramSourceProduct *product_out,
                              XrCliCanonicalSourceDiagnostic *diagnostic_out) {
    if (product_out)
        memset(product_out, 0, sizeof(*product_out));
    clear_diagnostic(diagnostic_out);
    if (!product_out || !request_valid(request))
        return reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_INVALID_INPUT,
                      "canonical source request is incomplete");

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(request->compiler_host);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(request->compiler_host);
    if (!session || !registry)
        return reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_INVALID_INPUT,
                      "compiler host lacks a source session or module registry");

    char *canonical_path = xr_realpath(request->entry_source_path);
    if (!canonical_path)
        return reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_SOURCE_REJECTED,
                      "entry source path cannot be canonicalized");

    xr_module_system_init_with_script(request->compiler_host, canonical_path);
    XrCliGraphAuthority authority = {0};
    char authority_error[XR_CLI_CANONICAL_SOURCE_DIAGNOSTIC_SIZE] = {0};
    if (!xr_cli_graph_authority_open(&authority, registry, canonical_path, authority_error,
                                     sizeof(authority_error))) {
        XrCliCanonicalSourceStatus status =
            reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_AUTHORITY_REJECTED, "%s",
                   authority_error[0] ? authority_error : "source authority is unavailable");
        xr_free(canonical_path);
        return status;
    }

    char *module_identity = NULL;
    char *logical_path = NULL;
    XrFingerprint source_fingerprint = {{0}};
    XrCliCanonicalSourceStatus status = XR_CLI_CANONICAL_SOURCE_OK;
    if (!xr_module_identity_from_source(&authority.entry_authority, canonical_path,
                                        &module_identity, &logical_path)) {
        status = reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_AUTHORITY_REJECTED,
                        "entry source is outside its exact module authority");
        goto cleanup;
    }
    if (!read_source_fingerprint(canonical_path, &source_fingerprint, authority_error,
                                 sizeof(authority_error))) {
        status =
            reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_SOURCE_REJECTED, "%s", authority_error);
        goto cleanup;
    }

    xr_compiler_session_set_native_package_plan(
        session, authority.project ? authority.project->native_plan : NULL);
    XrProgramSourceBuildInput input = {
        .schema_version = XR_PROGRAM_SOURCE_BUILD_SCHEMA_VERSION,
        .max_modules = XR_PROGRAM_SOURCE_BUILD_DEFAULT_MAX_MODULES,
        .session = session,
        .resolver = authority.resolver,
        .entry_source_path = canonical_path,
        .entry_authority = &authority.entry_authority,
        .entry =
            {
                .kind = request->entry_kind,
                .module_identity = module_identity,
                .function_name = request->entry_function,
                .source_content_fingerprint = source_fingerprint,
            },
        .source_profile = request->source_profile,
        .semantic_profile_fingerprint = request->semantic_profile_fingerprint,
    };
    XrProgramSourceDiagnostic build_diagnostic;
    XrProgramSourceBuildStatus build =
        xr_program_source_build(&input, product_out, &build_diagnostic);
    if (build != XR_PROGRAM_SOURCE_BUILD_OK) {
        if (diagnostic_out)
            diagnostic_out->build = build_diagnostic;
        status = reject(diagnostic_out, XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED, "%s",
                        build_diagnostic.message[0] ? build_diagnostic.message
                                                    : xr_program_source_build_status_name(build));
    }

cleanup:
    xr_free(logical_path);
    xr_free(module_identity);
    xr_cli_graph_authority_close(&authority);
    xr_free(canonical_path);
    return status;
}

const char *xr_cli_canonical_source_status_name(XrCliCanonicalSourceStatus status) {
    switch (status) {
        case XR_CLI_CANONICAL_SOURCE_OK:
            return "ok";
        case XR_CLI_CANONICAL_SOURCE_INVALID_INPUT:
            return "invalid-input";
        case XR_CLI_CANONICAL_SOURCE_AUTHORITY_REJECTED:
            return "authority-rejected";
        case XR_CLI_CANONICAL_SOURCE_SOURCE_REJECTED:
            return "source-rejected";
        case XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED:
            return "build-rejected";
    }
    return "unknown";
}
