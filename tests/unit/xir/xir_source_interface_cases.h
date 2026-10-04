/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_interface_cases.h - Source catalogs and declaration-context proofs
 */
#ifndef XIR_SOURCE_INTERFACE_CASES_H
#define XIR_SOURCE_INTERFACE_CASES_H
#include "xir/xxir_interface.h"

static void source_interface_context_cases(XrXirSourceRequest *request) {
    const char *sources[] = {
        "interface I<T> { values()->Array<T> }\nstruct Box<T> { values:Array<T> }\n",
        "interface I<U> {}\nstruct C<T> { value:T; "
            "static constrained<U>(value:U)->U where T:I<U> { return value } "
            "static identity(value:T)->T { return value } }\n",
        "interface I { get<T>(x:T)->T }\n"
    };
    for (uint32_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        write_source(request->entry_path,sources[i]);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status != XR_XIR_OK) fprintf(stderr,"interface context %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
        if (i == 1) {
            const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
            const XrXirSourceDeclaration *owner = declaration(view,"C",0); CHECK(owner);
            const XrXirSourceDeclaration *method = declaration(view,"constrained",owner->id);
            const XrXirSourceDeclaration *plain = declaration(view,"identity",owner->id);
            CHECK(method && plain && owner->generic_parameter_count == 1);
            CHECK(!owner->generic_constraints[0].interface_count && !plain->generic_constraints[0].interface_count);
            CHECK(method->generic_parameter_count == 2 && method->generic_constraints[0].interface_count == 1);
            CHECK(method->generic_constraints[0].interfaces[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE + 1);
        }
        xr_xir_compile_source_result_free(&result);
    }
    char library[XR_TEST_PATH_MAX];
    CHECK(snprintf(library,sizeof(library),"%s/interface-scope.xr",request->authority->physical_root) > 0);
    write_source(library,"export interface I<T> { value()->T }\n");
    write_source(request->entry_path,
        "import { I as Alias } from \"./interface-scope\"\nimport \"./interface-scope\" as lib\n"
        "fn take<T:Alias<U>,U>(x:T,y:U)->U{return y}\n"
        "fn forward<U,T:lib.I<U>>(x:T,y:U)->U{return take<T,U>(x,y)}\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_compile_source_check(request, &result, NULL, NULL) == XR_XIR_OK && result.checked);
    xr_xir_compile_source_result_free(&result);
    write_source(library,"interface I<T> { value()->T }\n");
    CHECK(xr_xir_compile_source_check(request, &result, NULL, NULL) != XR_XIR_OK && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result); CHECK(xr_test_unlink(library) == 0);
    write_source(request->entry_path,"interface I { value()->Missing }\n");
    XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(request, &result, &diagnostic, NULL) == XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    CHECK(diagnostic.line == 1 && diagnostic.column > 0);
    xr_xir_compile_source_result_free(&result);
}
static void source_interface_facts(XrXirSourceRequest *request) {
    const char *source =
        "interface Forward<T:I<U>,U> {}\n"
        "interface I<V> { get()->V }\n"
        "fn take<X:I<Y>,Y>(x:X,y:Y)->Y { return y }\n"
        "fn forward<U,T:I<U>&I<U>>(x:T,y:U)->U { return take<T,U>(x,y) }\n"
        "const answer=41\n";
    write_source(request->entry_path,source);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(request->context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local = *request; local.session = session;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&local, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"interface source: %u at %d:%d %s\n",
        status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const XrXirModule *owned = xr_xir_compile_artifact_module(result.checked);
    CHECK(owned->types && owned->types->interfaces && owned->types->interfaces->count == 2);
    CHECK(owned->function_count == 4);
    xr_compile_session_free(session);
    CHECK(xr_xir_compile_artifact_verify(result.checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL;
    write_source(request->entry_path,"const replaced=0\n");
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *forward = declaration(view,"forward",0);
    const XrXirSourceDeclaration *requirement = declaration(view,"I",0);
    CHECK(forward && requirement && forward->generic_parameter_count == 2);
    const XrXirConstraint *constraint = &forward->generic_constraints[1];
    CHECK(constraint->interface_count == 1 && constraint->interfaces[0].argument_count == 1);
    CHECK(constraint->interfaces[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
    const XrXirSourceDeclaration *member = declaration(view,"get",requirement->id);
    CHECK(member && member->kind == XR_XIR_SOURCE_MEMBER && member->parameter_count == 0);
    CHECK(member->type.known && member->type.type == XR_XIR_TYPE_PARAMETER_BASE);
    CHECK(member->type.generic_owner == requirement->id && member->generic_parent == requirement->id);
    CHECK(view->types && view->types->interfaces && view->types->interfaces->count == 2);
    CHECK(!memcmp(view->types->interfaces->declarations[1].methods[0].name.bytes,"get",3));
    xr_xir_compile_source_result_free(&result);
    const char *rejected[] = {
        "interface I { get()->i64 }\nfn unused<T>(x:T)->i64 { return x.get() }\n",
        "interface I<T> {}\nfn unused<T:I>(x:T)->T { return x }\n",
        ("interface I<T> {}\nfn take<T:I<U>,U>(x:T,y:U)->U { return y }\n"
            "fn bad<T,U>(x:T,y:U)->U { return take<T,U>(x,y) }\n"),
        "interface I { get()->i64 }\nstruct S implements I { static get()->i64 { return 41 } }\n",
        "interface I { value:i64 }\n"
    };
    for (uint32_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        write_source(request->entry_path,rejected[i]);
        CHECK(xr_xir_compile_source_check(request, &result, &diagnostic, NULL) != XR_XIR_OK && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
    source_interface_context_cases(request);
}
#endif // XIR_SOURCE_INTERFACE_CASES_H
