/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_query_schema.c - Generic display facts outside executable types
 */
#define main original_reference_main
#include "p09_reference/test_value_reference.c"
#undef main
#include "xir/xxir_types.h"

int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REFERENCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_REFERENCE_FIXTURES "/array_return_logical_copy.xr",
        &authority, &owner.context, XR_REFERENCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(&request, &result, &diagnostic, NULL) == XR_XIR_OK);
    const XrXirModule *checked = xr_xir_compile_artifact_module(result.checked);
    CHECK(checked && !checked->generics && checked->types);
    for (uint32_t t = 0; t < checked->types->count; ++t) CHECK(!checked->types->nodes[t].parameter_span);
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL; xr_compile_session_free(session);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(view && view->complete && view->types);
    unsigned found = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *member = &view->declarations[d];
        if (member->kind != XR_XIR_SOURCE_MEMBER || strcmp(member->name, "reverse")) continue;
        const XrXirTypeNode *array = xr_xir_type_node(view->types, member->type.type);
        CHECK(member->type.known && member->type.generic_owner == member->generic_parent &&
            member->generic_parent_count == 1 && member->mutable && !member->parameter_count);
        CHECK(array && array->kind == XR_XIR_TYPE_ARRAY && array->element == XR_XIR_TYPE_PARAMETER_BASE && array->parameter_span == 1);
        CHECK(!strcmp(member->signature, "() -> Array<T>")); ++found;
    }
    CHECK(found == 1); xr_xir_compile_source_result_free(&result); source_fixture_owner_free(&owner);
    puts("Array reverse: closed executable types, owned full Array<T> query after producer destruction PASS"); return 0;
}
