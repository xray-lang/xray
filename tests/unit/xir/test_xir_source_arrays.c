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
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_array_cases.h"
#include "xir_source_array_reordering_cases.h"
#include "xir_source_array_pipeline.h"
static void source_array_original(const char *checked_output, const char *bounds_output) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(compiler.context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ARRAY_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_ARRAY_SOURCE_FIXTURES "/root.xr", &authority, &compiler.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "array source %u %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_compile_session_free(session); session = NULL;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    unsigned members = 0, nominal_members = 0, length = 0, ordinals = 0, names = 0, texts = 0;
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
            const XrXirSourceDeclaration *owner = NULL;
            for (uint32_t parent = 0; parent < view->declaration_count; ++parent)
                if (view->declarations[parent].id == decl->parent) owner = &view->declarations[parent];
            CHECK(owner && owner->kind == XR_XIR_SOURCE_TYPE && !strcmp(owner->name,"ItemChoice"));
            CHECK(decl->type.known && !decl->parameter_count);
            if (!strcmp(decl->name,"ordinal")) { CHECK(decl->type.type == XR_XIR_I64); ++ordinals; }
            else if (!strcmp(decl->name,"name")) { CHECK(decl->type.type == XR_XIR_STRING); ++names; }
            else { CHECK(!strcmp(decl->name,"toString") && decl->type.type == XR_XIR_STRING); ++texts; }
        }
    }
    CHECK(members == 3 && nominal_members == 5 && length == 1 && ordinals == 1 && names == 1 && texts == 1);
    xr_xir_compile_source_result_free(&result);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    if (checked_output) {
        FILE *file = fopen(checked_output, "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t functions[ARRAY_FUNCTION_COUNT]; source_array_find(xr_xir_compile_artifact_module(lowered), functions);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_program_cases(program, functions); program = NULL;
    CHECK(xr_compile_session_new(compiler.context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    request.session = session; request.entry_path = XR_ARRAY_SOURCE_FIXTURES "/bounds.xr";
    CHECK(xr_xir_compile_source_check(&request, &result, &diagnostic, NULL) == XR_XIR_OK);
    checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); session = NULL;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    if (bounds_output) {
        FILE *file = fopen(bounds_output, "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_compile_checked_packet_free(&packet);
    lowered = source_array_lower(checked);
    uint32_t entry = xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_sticky_bounds(program, entry); program = NULL;
    source_fixture_owner_free(&compiler);
    puts("Source Array VM: golden, COW, root rebinding, snapshots, result ownership and physical release passed");
}

static void source_array_reordering(const char *checked_output) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(compiler.context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ARRAY_REORDER_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_ARRAY_REORDER_SOURCE_FIXTURES "/root.xr", &authority,
        &compiler.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "array reorder source %u %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_compile_session_free(session);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    unsigned members = 0, reverse = 0, unshift = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind != XR_XIR_SOURCE_MEMBER || !decl->native_identity) continue;
        CHECK(decl->signature && decl->range.module < view->module_count && decl->range.line > 0); ++members;
        if (!strcmp(decl->name, "reverse")) {
            CHECK(decl->native_identity == 18 && decl->mutable && decl->exported && !decl->parameter_count);
            CHECK(!strcmp(decl->signature, "() -> Array<T>") && !decl->type.known); ++reverse;
        } else if (!strcmp(decl->name, "unshift")) {
            CHECK(decl->native_identity == 10 && decl->mutable && decl->exported && decl->parameter_count == 1);
            CHECK(!strcmp(decl->signature, "(value: T)") && decl->type.known && decl->type.type == XR_XIR_UNIT);
            CHECK(decl->parameters[0].known && decl->parameters[0].generic_owner == decl->parent);
            CHECK(decl->parameters[0].type == XR_XIR_TYPE_PARAMETER_BASE); ++unshift;
        }
    }
    CHECK(members == 4 && reverse == 1 && unshift == 1);
    xr_xir_compile_source_result_free(&result);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    if (checked_output) {
        FILE *file = fopen(checked_output, "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t functions[REORDER_FUNCTION_COUNT]; source_array_reorder_find(xr_xir_compile_artifact_module(lowered), functions);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_reordering_program_cases(program, functions);
    source_fixture_owner_free(&compiler);
    puts("Source Array reordering VM: independent program, transaction, suspension and physical release passed");
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 4);
    source_array_original(argc == 4 ? argv[1] : NULL, argc == 4 ? argv[2] : NULL);
    source_array_reordering(argc == 4 ? argv[3] : NULL);
    return 0;
}
