/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_constructed_pipeline.h - Owned Source proof for constructed Task results
 */
#ifndef XIR_TASK_CONSTRUCTED_PIPELINE_H
#define XIR_TASK_CONSTRUCTED_PIPELINE_H
#include "xir/xxir_types.h"
static const char *constructed_names[] = {"array", "nested", "nullable", "nullable_array", "tuple", "enum", "atomic", "task"};
enum { CONSTRUCTED_CASES = sizeof(constructed_names) / sizeof(*constructed_names) };
static XrXirArtifact *constructed_lower(const XrXirCompileContext *context,
    uint32_t entries[CONSTRUCTED_CASES][2], bool baseline) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_CONSTRUCTED_FIXTURES};
    XrXirSourceRequest request = {session, XR_TASK_CONSTRUCTED_FIXTURES "/root.xr", &authority,
        context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL; XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "Task constructed Source status=%u %s\n", status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && !failure);
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic); xr_xir_compile_artifact_free(closed);
    if (baseline) {
        CHECK(status == XR_XIR_UNSUPPORTED && !lowered);
        printf("TASK_CONSTRUCTED_BASELINE Source/Checked73/specialization=OK Lowered=UNSUPPORTED function=%u instruction=%u\n",
            diagnostic.function, diagnostic.instruction); return NULL;
    }
    if (status != XR_XIR_OK) fprintf(stderr, "Task constructed Lowered status=%u function=%u instruction=%u\n", status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && lowered);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    for (unsigned n = 0; n < CONSTRUCTED_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode) {
        char name[64]; unsigned matches = 0;
        CHECK(snprintf(name, sizeof(name), "%s_%s", mode ? "handle" : "await", constructed_names[n]) > 0);
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
            if (identity->exported && identity->module == module->declarations->root_module && !identity->nominal_owner &&
                function->name_length == strlen(name) && !memcmp(function->name, name, function->name_length)) {
                CHECK(!function->parameter_count); entries[n][mode] = f; ++matches;
            }
        }
        CHECK(matches == 1);
    }
    return lowered;
}
#endif // XIR_TASK_CONSTRUCTED_PIPELINE_H
