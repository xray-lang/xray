/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_array_query_types.c - Query-only Array binders cannot enter executable types
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Array query %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static void array_query_closed(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    CHECK(module && module->function_count == 3);
    for (uint32_t t = 0; module->types && t < module->types->count; ++t)
        CHECK(!module->types->nodes[t].parameter_span);
}
static void array_query_schema(const XrXirSourceResult *result, const char *member) {
    array_query_closed(result->checked);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    if (!member) return;
    unsigned matches = 0;
    const XrXirModule *module = xr_xir_compile_artifact_module(result->checked);
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *declaration = &view->declarations[d];
        if (!declaration->native_identity || strcmp(declaration->name, member)) continue;
        CHECK(!matches++ && declaration->type.known && declaration->generic_parent_count == 1);
        CHECK((uint32_t)declaration->type.type == XR_XIR_CONSTRUCTED_TYPE_BASE + module->types->count);
        const XrXirTypeNode *type = xr_xir_type_node(view->types, declaration->type.type);
        CHECK(type && type->kind == XR_XIR_TYPE_ARRAY && type->parameter_span == 1);
        CHECK(type->element == XR_XIR_TYPE_PARAMETER_BASE && view->types->count == module->types->count+1);
        CHECK(declaration->parameter_count == (!strcmp(member, "reserve") ? 1u : 0u));
    }
    CHECK(matches == 1);
}
static void array_query_case(const XrXirCompileContext *context, const char *body, const char *member) {
    XrCompileResourceStats baseline = {0}; CHECK(xr_compile_resources_stats(context->resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "p20-array-query", NULL};
    XrXirSourceRequest request = {session, NULL, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    size_t bytes = strlen(body); char *text = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, bytes+1, 1, (void **)&text) == XR_COMPILE_RESOURCE_OK);
    memcpy(text, body, bytes); XrXirSourceText source = {NULL, text, bytes};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check_text(&request, &source, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "Array query status=%u at=%d:%d %s\n", status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    xr_compile_session_free(session); memset(text, 0xCC, bytes); xr_compile_resources_free(text);
    array_query_schema(&result, member);
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL;
    XrXirArtifact *checked = NULL; CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    array_query_closed(checked); CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    array_query_closed(lowered); XrXirCSource c = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "array_query_fixture", 4194304, &c) == XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text, "({") && !strstr(c.text, "xr_xir_vm"));
    xr_xir_compile_c_source_free(&c); xr_xir_compile_artifact_free(lowered);
    XrCompileResourceStats after = {0}; CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes == baseline.live_bytes && after.allocated_bytes > baseline.allocated_bytes && after.work > baseline.work);
    printf("Array query member=%s SourceSessionTextQueryDead CheckedPacketRead Lowered CGen liveBaseline\n", member ? member : "control");
}
int main(void) {
    const char *bodies[] = {
        "export fn inspect()->i64 { return 42 }",
        "export fn inspect()->Array<i64> { return [3,1,2] }",
        "export fn inspect()->Array<i64> { var values:Array<i64>=[3,1,2]; return values }",
        "export fn inspect()->i64 { var values:Array<i64>=[3,1,2]; values.reverse(); return len(values) }",
        "export fn inspect()->Array<i64> { var values:Array<i64>=[3,1,2]; return values.reverse() }",
        "export fn inspect()->i64 { var values:Array<i64>=[3,1,2]; values.reserve(8); return len(values) }"
    };
    const char *members[] = {NULL, NULL, NULL, "reverse", "reverse", "reserve"};
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    for (unsigned i = 0; i < 6; ++i) array_query_case(context, bodies[i], members[i]);
    effects_source_owners_free(); puts("Array query binders stay query-owned, executable pools closed, physical0/0"); return 0;
}
