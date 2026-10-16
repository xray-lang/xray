/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_source.h - Source-owned hash type facts and executable carriers
 */
#ifndef P09_HASH_SOURCE_H
#define P09_HASH_SOURCE_H
#include "xir/xxir_source.h"
#include "shared/xnative_declaration.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
static const char hash_source_program[] =
    "export fn entry()->i64{return 42}\n"
    "export fn echo(x:Map<string,Array<i64>>)->Map<string,Array<i64>>{return x}\n"
    "export fn relay(x:Map<string,Array<i64>>)->Map<string,Array<i64>>{return echo(x)}\n"
    "export fn setecho(x:Set<f64>)->Set<f64>{return x}\n"
    "export fn setrelay(x:Set<f64>)->Set<f64>{return setecho(x)}\n";
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
static void hash_source_query(const XrXirSourceView *view) {
    CHECK(view && view->complete);
    unsigned map = 0, set = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *record = &view->declarations[i];
        if (record->kind != XR_XIR_SOURCE_TYPE || (record->native_identity != XR_NATIVE_DECLARATION_MAP && record->native_identity != XR_NATIVE_DECLARATION_SET)) continue;
        bool is_map = record->native_identity == XR_NATIVE_DECLARATION_MAP;
        CHECK(!strcmp(record->name, is_map ? "Map" : "Set") && record->exported);
        CHECK(record->generic_parameter_count == (is_map ? 2u : 1u) && record->generic_constraints);
        CHECK(record->generic_constraints[0].markers == XR_XIR_CONSTRAINT_HASHABLE);
        if (is_map) CHECK(!record->generic_constraints[1].markers);
        CHECK(record->range.module < view->module_count && record->range.line == 2);
        CHECK(!strcmp(view->modules[record->range.module].path, is_map ? "stdlib/types/map.xr" : "stdlib/types/set.xr"));
        uint32_t parameters = 0;
        for (uint32_t p = 0; p < view->declaration_count; ++p) {
            const XrXirSourceDeclaration *parameter = &view->declarations[p];
            if (parameter->parent != record->id || parameter->kind != XR_XIR_SOURCE_TYPE_PARAMETER) continue;
            CHECK(parameters < record->generic_parameter_count && parameter->type.known);
            CHECK(!strcmp(parameter->name, is_map ? (parameters ? "V" : "K") : "T"));
            CHECK(parameter->type.generic_owner == record->id &&
                parameter->type.type == (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + parameters));
            ++parameters;
        }
        CHECK(parameters == record->generic_parameter_count);
        if (is_map) ++map; else ++set;
    }
    CHECK(map == 1 && set == 1);
}
static void hash_source_entries(const XrXirModule *module, uint32_t entries[4]) {
    const char *names[] = {"echo", "relay", "setecho", "setrelay"};
    for (unsigned n = 0; n < 4; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) {
                CHECK(entries[n] == UINT32_MAX); entries[n] = f;
            }
        CHECK(entries[n] != UINT32_MAX);
    }
}
static XrXirArtifact *hash_source_replay(const XrXirCompileContext *context, XrXirSourceResult *source) {
    XrXirCheckedPacket packet = {0}; XrXirArtifact *read = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(source->checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xcc, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read, &specialized, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_lower(specialized, &(XrXirTarget){XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}
static XrXirArtifact *hash_source_lower(const XrXirCompileContext *context) {
    XrXirSourceResult source = {0};
    CHECK(hash_source_check(context, hash_source_program, &source) == XR_XIR_OK);
    hash_source_query(xr_xir_compile_source_snapshot_view(source.snapshot));
    XrXirArtifact *lowered = hash_source_replay(context, &source);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entries[4]; hash_source_entries(module, entries);
    return lowered;
}
#endif // P09_HASH_SOURCE_H
