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
#include "xir_mixed_array_query_prepare.h"
static int array_query_main(void) {
    ArrayQueryCompileResult result = array_query_compile_operation(SIZE_MAX, array_query_compile_caps(), true);
    CHECK(result.status == XR_XIR_OK && result.completed == 6 && !result.injected);
    printf("Array query normal sites=%zu allocated=%llu peak=%llu work=%llu, oneFiniteOwner physical0/0\n", result.sites,
        (unsigned long long)result.stats.allocated_bytes, (unsigned long long)result.stats.peak_bytes,
        (unsigned long long)result.stats.work);
    puts("Array query binders stay query-owned, executable pools closed, physical0/0"); return 0;
}
#ifndef XR_ARRAY_QUERY_CUSTOM_MAIN
int main(void) { return array_query_main(); }
#endif
