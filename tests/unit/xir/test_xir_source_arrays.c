/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_arrays.c - Real source Array values through the single pipeline
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_source_array_cases.h"
#include "xir_source_array_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 3);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ARRAY_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_ARRAY_SOURCE_FIXTURES "/root.xr", &authority, NULL, NULL, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "array source %u %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_compiler_session_delete(session);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    unsigned members = 0, nominal_members = 0, length = 0, ordinals = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind == XR_XIR_SOURCE_MEMBER && decl->native_identity) {
            CHECK(decl->native_identity && decl->signature && decl->signature[0] == '(');
            CHECK(decl->range.module < view->module_count && decl->range.line > 0); ++members;
        }
        if (decl->kind == XR_XIR_SOURCE_MEMBER && !decl->native_identity) {
            CHECK(decl->range.module < view->module_count && decl->range.line > 0); ++nominal_members;
        }
        if (decl->kind == XR_XIR_SOURCE_INTRINSIC && decl->native_identity) {
            CHECK(decl->native_identity == 6 && !strcmp(decl->name, "len"));
            CHECK(decl->parameter_count == 1 && !decl->parameters[0].known); ++length;
        }
        if (decl->kind == XR_XIR_SOURCE_INTRINSIC && !decl->native_identity) {
            CHECK(!strcmp(decl->name, "ordinal")); ++ordinals;
        }
    }
    CHECK(members == 3 && nominal_members == 5 && length == 1 && ordinals == 1); xr_xir_source_result_free(&result);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    if (argc == 3) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t functions[ARRAY_FUNCTION_COUNT]; source_array_find(xr_xir_artifact_module(lowered), functions);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK && !lowered);
    source_array_program_cases(program, functions);
    session = xr_compiler_session_new(NULL); CHECK(session);
    request.session = session; request.entry_path = XR_ARRAY_SOURCE_FIXTURES "/bounds.xr";
    CHECK(xr_xir_source_check(&request, &result, &diagnostic) == XR_XIR_OK);
    checked = result.checked; result.checked = NULL;
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    if (argc == 3) {
        FILE *file = fopen(argv[2], "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_checked_packet_free(&packet);
    lowered = source_array_lower(checked);
    uint32_t entry = xr_xir_artifact_module(lowered)->declarations->entry_function;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK && !lowered);
    source_array_sticky_bounds(program, entry);
    puts("Source Array VM: golden, COW, root rebinding, snapshots, result ownership and physical release passed");
    return 0;
}
