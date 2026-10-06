/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_entries_rejections.h - Receiver and definition authority boundaries
 */
#ifndef XIR_ARRAY_ENTRIES_REJECTIONS_H
#define XIR_ARRAY_ENTRIES_REJECTIONS_H
static void entries_rejections(bool probe) {
    static const char *const cases[]={"arity","typeargs","wrong_result","flatten","generic","private","function_value","unknown"};
    static const char *const reasons[]={
        "Array entries takes no arguments","Array entries takes no arguments","expression cannot satisfy its declared type",
        "expression cannot satisfy its declared type","expression cannot satisfy its declared type","method requires its declaration owner",
        "member receiver is not a nominal value","Array member has no admitted value-operation contract"};
    for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);++n) {
        instance_compile_zero();instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};const XrCompileResourceLimits limits=entries_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline=entries_stats(&context).live_bytes;
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        char name[1024];CHECK(snprintf(name,sizeof(name),"%s/rejected/%s/root.xr",XR_ENTRIES_FIXTURES,cases[n])>0);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ENTRIES_FIXTURES};
        XrXirSourceRequest request={session,name,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
        printf("entries rejected%u %s status%u line%d:%d reason=%s\n",n,cases[n],status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(!result.checked&&!result.snapshot);
        if(!probe)CHECK(status==XR_XIR_BAD_TYPE&&diagnostic.status==status&&!strcmp(diagnostic.message,reasons[n]));
        xr_compile_resources_free(path);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(entries_stats(&context).live_bytes==baseline);xr_compile_resources_release(context.resources);
        instance_compile_zero();CHECK(!runtime_live&&!runtime_bytes);
    }
}
#endif
