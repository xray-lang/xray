/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_rejections.h - Exact receiver and definition contracts
 *
 * KEY CONCEPT:
 *   A rejected Source request publishes neither Checked nor a partial snapshot.
 */
#ifndef XIR_ARRAY_RESIZE_REJECTIONS_H
#define XIR_ARRAY_RESIZE_REJECTIONS_H
static void resize_rejections(void) {
    static const char *const cases[]={"const","read","temporary","arity0","arity1","arity3","typearg","ref_fill","bad_length","bad_fill","flatten","const_field","private_field","generic"};
    static const char *const reasons[]={
        "value mutation requires a mutable named root","value mutation requires a mutable named root","value mutation requires a mutable named root",
        "Array resize requires length and fill","Array resize requires length and fill","Array resize requires length and fill","Array resize requires length and fill",
        "Array resize arguments use ordinary READ values","expression cannot satisfy its declared type","expression cannot satisfy its declared type",
        "expression cannot satisfy its declared type","field access is not permitted","field access is not permitted","expression cannot satisfy its declared type"};
    _Static_assert(sizeof(cases)/sizeof(cases[0])==sizeof(reasons)/sizeof(reasons[0]),"One exact reason per original source");
    for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);++n) {
        instance_compile_zero();instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};const XrCompileResourceLimits limits=resize_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline=resize_stats(&context).live_bytes;
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        char name[1024];CHECK(snprintf(name,sizeof(name),"%s/rejected/%s/root.xr",XR_RESIZE_FIXTURES,cases[n])>0);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_RESIZE_FIXTURES};
        XrXirSourceRequest request={session,name,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
        if(status!=XR_XIR_BAD_TYPE||strcmp(diagnostic.message,reasons[n]))
            fprintf(stderr,"rejection %s status%u line%d reason=%s\n",cases[n],status,diagnostic.line,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE&&diagnostic.status==status&&!result.checked&&!result.snapshot);
        CHECK(!strcmp(diagnostic.message,reasons[n]));
        printf("resize negative%u %s status%u reason=%s\n",n,cases[n],status,diagnostic.message);
        xr_compile_resources_free(path);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(resize_stats(&context).live_bytes==baseline);xr_compile_resources_release(context.resources);
        instance_compile_zero();CHECK(!runtime_live&&!runtime_bytes);
    }
}
#endif
