/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_go_source.c - Source-owned Task construction and continuations
 *
 * KEY CONCEPT:
 *   Source checks use authentic declaration proof before any execution exists.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "shared/xnative_declaration.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"
typedef struct SourceTaskFixture { const char *name; unsigned goes, waits; } SourceTaskFixture;
static const SourceTaskFixture source_task_fixtures[] = {
    {"integer", 1, 1}, {"string", 1, 2}, {"generic", 1, 1},
    {"context_ordinary", 0, 0}, {"context_go", 1, 1}, {"context_existing", 1, 1},
    {"context_spawn", 1, 1}, {"grouped", 1, 1}, {"import_default", 1, 1},
    {"const_state", 1, 1}, {"local_storage", 1, 1}, {"unknown_task", 1, 2},
    {"value_error", 1, 1}, {"shadow_class", 1, 1}, {"context_match", 2, 1},
    {"context_match_block", 2, 1}, {"context_match_existing", 1, 1}
};
static void source_task_shape(const XrXirModule *module, const SourceTaskFixture *fixture) {
    CHECK(module && module->stage == XR_XIR_CHECKED);
    unsigned actual_go = 0, actual_wait = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            const XrXirInstruction *op = &module->functions[f].instructions[i];
            if (op->op == XR_XIR_GO) { ++actual_go; CHECK(xr_xir_task_element(module->types, op->type)); }
            if (op->op == XR_XIR_TASK_AWAIT) { ++actual_wait; CHECK(op->type == XR_XIR_UNIT && op->targets[0] != op->targets[1]); }
        }
    CHECK(actual_go == fixture->goes && actual_wait == fixture->waits);
    if (!strcmp(fixture->name, "string")) {
        unsigned exact = 0;
        for (uint32_t l = 0; l < module->declarations->literal_count; ++l) {
            const XrXirLiteral *literal = &module->declarations->literals[l];
            if (literal->length == 3 && !memcmp(literal->bytes, "A\0B", 3)) ++exact;
        }
        CHECK(exact == 1);
    }
}
/* Observe the existing operation without allocating or changing its budget. */
static XrXirStatus source_task_compiler_record(const XrXirCompileContext *context,
    const SourceTaskFixture *fixture, XrXirStatus status) {
    if (source_program_compile_fail_at != SIZE_MAX) {
        XrCompileResourceStats stats = library_compile_stats(context);
        printf("{\"row\":\"checked_compiler_fi\",\"case\":\"%s\",\"ordinal\":%zu,"
            "\"attempts\":%zu,\"injected\":%u,\"status\":%u,\"live_bytes\":%llu}\n",
            fixture->name, source_program_compile_fail_at, source_program_compile_attempts,
            source_program_compile_injected ? 1u : 0u, status, (unsigned long long)stats.live_bytes);
    }
    return status;
}
static XrXirStatus source_task_graph(const XrXirCompileContext *context, void *opaque) {
    const SourceTaskFixture *fixture = opaque;
    XrCompilerSession *session = NULL;
    XrCompilerSessionStatus created = xr_compile_session_new(context->resources, &session);
    if (created != XR_COMPILER_SESSION_OK)
        return source_task_compiler_record(context, fixture,
            created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_GO_SOURCE_FIXTURES, fixture->name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirCheckedPacket packet = {0}; XrXirArtifact *read = NULL, *specialized = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    xr_compile_session_free(session); session = NULL;
    if (status != XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if (source_program_compile_fail_at == SIZE_MAX)
            fprintf(stderr, "%s status=%u location=%d:%d message=%s\n", fixture->name, status,
                diagnostic.line, diagnostic.column, diagnostic.message);
        goto done;
    }
    CHECK(result.checked && result.snapshot && !failure);
    source_task_shape(xr_xir_compile_artifact_module(result.checked), fixture);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(view && view->complete);
    if (fixture->goes) {
        unsigned authentic = 0;
        for (uint32_t d = 0; d < view->declaration_count; ++d) {
            const XrXirSourceDeclaration *record = &view->declarations[d];
            if (record->native_identity != XR_NATIVE_DECLARATION_TASK) continue;
            ++authentic;
            CHECK(!strcmp(record->name, "Task") && record->exported && record->generic_parameter_count == 1);
            CHECK(record->generic_constraints && record->generic_constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
            CHECK(record->range.module < view->module_count);
            CHECK(!strcmp(view->modules[record->range.module].identity, "xray-native:prelude/Task"));
        }
        CHECK(authentic == 1);
    }
    status = xr_xir_compile_checked_write(result.checked, &packet, NULL);
    if (status != XR_XIR_OK) { CHECK(!packet.bytes && !packet.length); goto done; }
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL;
    status = xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL);
    if (status != XR_XIR_OK) { CHECK(!read); goto done; }
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    source_task_shape(xr_xir_compile_artifact_module(read), fixture);
    status = xr_xir_compile_verify_v2(context, xr_xir_compile_artifact_module(read), xr_xir_compile_artifact_construction(read), NULL);
    if (status != XR_XIR_OK) goto done;
    status = xr_xir_compile_specialize(read, &specialized, NULL);
    if (status != XR_XIR_OK) { CHECK(!specialized); goto done; }
    xr_xir_compile_artifact_free(read); read = NULL;
    xr_xir_compile_source_result_free(&result);
    status = xr_xir_compile_verify_v2(context, xr_xir_compile_artifact_module(specialized), xr_xir_compile_artifact_construction(specialized), NULL);
done:
    xr_compile_resources_free(failure);
    xr_xir_compile_source_result_free(&result);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(read); xr_xir_compile_artifact_free(specialized);
    xr_compile_session_free(session);
    return source_task_compiler_record(context, fixture, status);
}
static void source_task_reject(const char *name, XrXirStatus expected) {
    LibraryCompileOwner owner = {0};
    CHECK(library_compile_owner_new(&owner, &library_compile_limits) == XR_XIR_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/rejected/%s.xr", XR_GO_SOURCE_FIXTURES, name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &owner.context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    fprintf(stderr, "reject %s status=%u location=%d:%d message=%s\n", name, status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == expected && !result.checked && !result.snapshot);
    if (!strcmp(name, "private_import")) CHECK(strstr(diagnostic.message, "requires an exported declaration"));
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); library_compile_owner_drop(&owner);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && (!strcmp(argv[1], "--compiler") || !strcmp(argv[1], "--compiler-measure"))) {
        const SourceTaskFixture *fixture = NULL;
        for (size_t f = 0; f < sizeof(source_task_fixtures) / sizeof(*source_task_fixtures); ++f)
            if (!strcmp(argv[2], source_task_fixtures[f].name)) fixture = &source_task_fixtures[f];
        if (!fixture) return 2;
        if (!strcmp(argv[1], "--compiler"))
            library_compile_operation_cases(fixture->name, source_task_graph, (void *) fixture);
        else {
            LibraryCompileOwner owner = {0};
            CHECK(library_compile_owner_new(&owner, &library_compile_limits) == XR_XIR_OK);
            source_program_compile_attempts = 0;
            CHECK(source_task_graph(&owner.context, (void *) fixture) == XR_XIR_OK);
            size_t sites = source_program_compile_attempts;
            XrCompileResourceStats measured = library_compile_stats(&owner.context);
            CHECK(sites && measured.live_bytes == owner.baseline.live_bytes);
            library_compile_owner_drop(&owner);
            printf("{\"row\":\"checked_compiler_measure\",\"case\":\"%s\",\"sites\":%zu,"
                "\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"live_bytes\":%llu}\n",
                fixture->name, sites, (unsigned long long)measured.allocated_bytes,
                (unsigned long long)measured.peak_bytes, (unsigned long long)measured.work,
                (unsigned long long)measured.live_bytes);
        }
    } else if (argc == 1) {
        for (size_t f = 0; f < sizeof(source_task_fixtures) / sizeof(*source_task_fixtures); ++f) {
            LibraryCompileOwner owner = {0}; CHECK(library_compile_owner_new(&owner, &library_compile_limits) == XR_XIR_OK);
            CHECK(source_task_graph(&owner.context, (void *) &source_task_fixtures[f]) == XR_XIR_OK);
            library_compile_owner_drop(&owner);
        }
        source_task_reject("generic_definition", XR_XIR_BAD_TYPE);
        source_task_reject("mutable_read", XR_XIR_BAD_TYPE);
        source_task_reject("mutable_helper", XR_XIR_BAD_TYPE);
        source_task_reject("cleanup_spawn", XR_XIR_BAD_TYPE);
        source_task_reject("cleanup_wait", XR_XIR_BAD_TYPE);
        source_task_reject("indirect", XR_XIR_BAD_TYPE);
        source_task_reject("construct", XR_XIR_BAD_TYPE);
        source_task_reject("await_scalar", XR_XIR_BAD_TYPE);
        source_task_reject("task_invariance", XR_XIR_BAD_TYPE);
        source_task_reject("shadow_await", XR_XIR_BAD_TYPE);
        source_task_reject("private_import", XR_XIR_BAD_STRUCTURE);
        source_task_reject("grouped_numeric", XR_XIR_BAD_TYPE);
    } else return 2;
    library_compile_observer_free();
    return 0;
}
