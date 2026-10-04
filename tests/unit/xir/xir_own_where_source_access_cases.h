/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_own_where_source_access_cases.h - Source-owned conditional access boundaries
 *
 * KEY CONCEPT:
 *   Describing a bound or a private field grants no import, proof, or construction authority.
 */
#ifndef OWN_WHERE_SOURCE_ACCESS_CASES_H
#define OWN_WHERE_SOURCE_ACCESS_CASES_H

/* Each check destroys its parser/compiler producer before the caller reads results. */
static XrXirSourceResult own_where_access_check(const XrXirSourceRequest *request,
    const char *source, XrXirStatus expected, const char *reason) {
    write_source(request->entry_path,source);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(request->context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local=*request; local.session=session;
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&local, &result, &diagnostic, NULL);
    xr_compile_session_free(session);
    if (status!=expected || (reason && !strstr(diagnostic.message,reason)))
        fprintf(stderr,"own where access: %u %d:%d %s\n",status,
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected && (!reason || strstr(diagnostic.message,reason)));
    if (expected==XR_XIR_OK) {
        CHECK(result.checked && result.snapshot);
        CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        CHECK(xr_xir_compile_artifact_verify(result.checked, NULL)==XR_XIR_OK);
    } else {
        CHECK(!result.checked && !result.snapshot);
        CHECK(!result.snapshot);
    }
    return result;
}
static void own_where_source_access_cases(XrXirSourceRequest *request) {
    char library[XR_TEST_PATH_MAX],bridge[XR_TEST_PATH_MAX];
    int n=snprintf(library,sizeof(library),"%s/own-where-library.xr",request->authority->physical_root);
    CHECK(n>0 && (size_t)n<sizeof(library));
    n=snprintf(bridge,sizeof(bridge),"%s/own-where-bridge.xr",request->authority->physical_root);
    CHECK(n>0 && (size_t)n<sizeof(bridge));
    const char *type_only=
        "import \"./own-where-library\" as lib\n"
        "interface Use<A> { map<U>(value:U)->U where U:lib.Evidence<Array<lib.Payload>> }\n";
    write_source(library,"export interface Evidence<A> {}\nexport struct Payload { private value:i64 }\n");
    XrXirSourceResult described=own_where_access_check(request,type_only,XR_XIR_OK,NULL);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(described.snapshot);
    CHECK(view->types && view->types->nominals && view->types->interfaces);
    bool saw_private=false;
    for (uint32_t d=0;d<view->types->nominals->count;++d) {
        const XrXirNominalDeclaration *record=&view->types->nominals->declarations[d];
        for (uint32_t f=0;f<record->field_count;++f)
            if (record->fields[f].flags & XR_XIR_FIELD_PRIVATE) saw_private=true;
    }
    CHECK(saw_private);
    xr_xir_compile_artifact_free(described.checked); described.checked=NULL;
    /* A live, complete descriptive snapshot must not enlarge a fresh check's authority. */
    XrXirSourceResult rejected=own_where_access_check(request,
        "import \"./own-where-library\" as lib\n"
        "interface Use<A> { map<U>(value:U)->U where U:lib.Evidence<Array<lib.Payload>> }\n"
        "fn forbidden()->lib.Payload{return lib.Payload{value:41}}\n",
        XR_XIR_BAD_TYPE,"field access is not permitted");
    xr_xir_compile_source_result_free(&rejected);
    CHECK(view->complete && view->types->nominals->count);
    xr_xir_compile_source_result_free(&described);
    /* Removing only field privacy provides a construction control. */
    write_source(library,"export interface Evidence<A> {}\nexport struct Payload { value:i64 }\n");
    XrXirSourceResult control=own_where_access_check(request,
        "import \"./own-where-library\" as lib\n"
        "interface Use<A> { map<U>(value:U)->U where U:lib.Evidence<Array<lib.Payload>> }\n"
        "fn permitted()->lib.Payload{return lib.Payload{value:41}}\n",XR_XIR_OK,NULL);
    xr_xir_compile_source_result_free(&control);
    write_source(library,"interface Evidence<A> {}\nexport struct Payload { value:i64 }\n");
    rejected=own_where_access_check(request,type_only,XR_XIR_BAD_STRUCTURE,"import requires an exported declaration");
    xr_xir_compile_source_result_free(&rejected);
    write_source(library,"export interface Evidence<A> {}\nstruct Payload { value:i64 }\n");
    rejected=own_where_access_check(request,type_only,XR_XIR_BAD_STRUCTURE,"import requires an exported declaration");
    xr_xir_compile_source_result_free(&rejected);
    /* The library exists in the graph via bridge; graph reachability is not an import. */
    write_source(library,"export interface Evidence<A> {}\nexport struct Payload { value:i64 }\n");
    write_source(bridge,"import \"./own-where-library\" as lib\nexport fn ping()->i64{return 41}\n");
    rejected=own_where_access_check(request,
        "import \"./own-where-bridge\" as bridge\n"
        "interface Use { map<U>(value:U)->U where U:lib.Evidence<i64> }\n",
        XR_XIR_BAD_TYPE,"constraint name does not resolve to an interface");
    xr_xir_compile_source_result_free(&rejected);
    CHECK(xr_test_unlink(bridge)==0 && xr_test_unlink(library)==0);
}
static void own_where_self_bound_cases(XrXirSourceRequest *request) {
    const char *positive=
        "interface Evidence<A> {}\n"
        "interface Self { map<U>(value:U)->U where U:Evidence<U> }\n"
        "fn forward<T:Self,V:Evidence<V>>(receiver:T,value:V)->V{return receiver.map<V>(value)}\n";
    XrXirSourceResult result=own_where_access_check(request,positive,XR_XIR_OK,NULL);
    xr_xir_compile_source_result_free(&result);
    result=own_where_access_check(request,
        "interface Evidence<A> {}\n"
        "interface Self { map<U>(value:U)->U where U:Evidence<U> }\n"
        "fn forward<T:Self,V>(receiver:T,value:V)->V{return receiver.map<V>(value)}\n",
        XR_XIR_BAD_TYPE,"method type argument does not prove");
    xr_xir_compile_source_result_free(&result);
}
#endif // OWN_WHERE_SOURCE_ACCESS_CASES_H
