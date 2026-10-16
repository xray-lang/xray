/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_source_check.h - Source producer teardown and independent Checked replay
 */
#ifndef P09_HASH_SOURCE_CHECK_H
#define P09_HASH_SOURCE_CHECK_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
static XrXirStatus hash_source_check(const XrXirCompileContext *context, const char *text,
    XrXirSourceResult *result) {
    char directory[XR_TEST_PATH_MAX] = "hash-source-XXXXXX", absolute[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(path, sizeof(path), "%s/root.xr", absolute) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    size_t length = strlen(text); CHECK(fwrite(text, 1, length, file) == length && !fclose(file));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, path, &authority, context, XR_P09_HASH_STDLIB,
        NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, result, &diagnostic, NULL);
    xr_compile_session_free(session);
    CHECK(!remove(path) && !xr_test_rmdir(directory));
    if (status != XR_XIR_OK) fprintf(stderr, "hash source status=%u at %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK ? result->checked && result->snapshot : !result->checked && !result->snapshot);
    return status;
}
static XrXirArtifact *hash_source_replay(const XrXirCompileContext *context, XrXirSourceResult *source) {
    XrXirCheckedPacket packet = {0}; XrXirArtifact *read = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(source->checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xcc, packet.length); xr_xir_compile_checked_packet_free(&packet);
    XrXirStatus specialized_status = xr_xir_compile_specialize(read, &specialized, NULL);
    if (specialized_status != XR_XIR_OK) {
        XrCompileResourceStats stats = {0};
        CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "Source specialize status=%u cumulative work=%llu allocated=%llu live=%llu\n", specialized_status,
            (unsigned long long)stats.work, (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.live_bytes);
    }
    CHECK(specialized_status == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    XrXirDiagnostic lower_diagnostic = {0};
    XrXirStatus lower_status = xr_xir_compile_lower(specialized,
        &(XrXirTarget){XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, &lowered, &lower_diagnostic);
    if (lower_status != XR_XIR_OK) {
        const XrXirModule *module = xr_xir_compile_artifact_module(specialized);
        XrCompileResourceStats stats = {0};
        CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "Source lower status=%u function=%u block=%u instruction=%u reason=%u work=%llu\n",
            lower_status, lower_diagnostic.function, lower_diagnostic.block, lower_diagnostic.instruction,
            lower_diagnostic.reason, (unsigned long long)stats.work);
        if (lower_diagnostic.function < module->function_count) {
            const XrXirFunction *function = &module->functions[lower_diagnostic.function];
            fprintf(stderr, "Source lower function=%.*s\n", (int)function->name_length, function->name);
            if (lower_diagnostic.instruction < function->instruction_count) {
                const XrXirInstruction *op = &function->instructions[lower_diagnostic.instruction];
                fprintf(stderr, "Source lower operation=%s type=%u args=%u,%u\n",
                    xr_xir_op_name(op->op), op->type, op->args[0], op->args[1]);
            }
        }
    }
    CHECK(lower_status == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}
#endif // P09_HASH_SOURCE_CHECK_H
