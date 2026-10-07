/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_rejections.h - Real invalid Source requests retain no partial output
 *
 * KEY CONCEPT:
 *   Ordinary generic declarations never gain access from a future instantiation.
 */
#ifndef XIR_ARRAY_CAPACITY_REJECTIONS_H
#define XIR_ARRAY_CAPACITY_REJECTIONS_H
static void capacity_rejections(void) {
    static const char *const cases[]={"const","read","temporary","const_field",
        "private_field","bad_capacity","bad_reserve","ref_argument","typearg",
        "generic","generic_capacity","property_write","missing_capacity",
        "extra_capacity","missing_reserve","extra_reserve","static_typearg",
        "u32_capacity","u32_reserve","shadow_missing"};
    static const char *const reasons[]={
        "value mutation requires a mutable named root",
        "value mutation requires a mutable named root",
        "value mutation requires a mutable named root",
        "field access is not permitted","field access is not permitted",
        "expression cannot satisfy its declared type","expression cannot satisfy its declared type",
        "Array method arguments use ordinary READ values","Array method requires its declared arguments",
        "receiver has no declared interface requirements","receiver has no declared interface requirements",
        "member receiver is not a nominal value",
        "Array static member requires its declared ordinary arguments",
        "Array static member requires its declared ordinary arguments",
        "Array method requires its declared arguments","Array method requires its declared arguments",
        "Array static member requires its declared ordinary arguments",
        "expression cannot satisfy its declared type","expression cannot satisfy its declared type",
        "unknown struct field"};
    _Static_assert(sizeof(cases)/sizeof(cases[0])==sizeof(reasons)/sizeof(reasons[0]),
        "Every invalid complete input has one independent semantic reason");
    /* These locations name the rejected receiver, member or argument token.
     * Array literal, generic-call and member-set factories retain no column. */
    static const int locations[][2]={
        {1,49},{1,39},{1,0},{2,55},{2,57},{1,56},{1,58},{1,58},{1,0},{1,46},
        {1,36},{1,0},{1,43},{1,43},{1,50},{1,50},{1,0},{1,70},{1,72},{3,60}};
    _Static_assert(sizeof(cases)/sizeof(cases[0])==sizeof(locations)/sizeof(locations[0]),
        "Every invalid complete input has one independent Source location");
    unsigned failures=0;
    for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);++n){
        instance_compile_zero();instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};const XrCompileResourceLimits limits=capacity_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline=capacity_stats(&context).live_bytes;
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        char name[1024];CHECK(snprintf(name,sizeof(name),"%s/rejected/%s/root.xr",XR_CAPACITY_FIXTURES,cases[n])>0);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CAPACITY_FIXTURES};
        XrXirSourceRequest request={session,name,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
        printf("capacity rejected%u %s status%u line%d:%d reason=%s\n",n,cases[n],status,diagnostic.line,diagnostic.column,diagnostic.message);
        const bool semantic=status==XR_XIR_BAD_TYPE&&diagnostic.status==status&&
            !result.checked&&!result.snapshot&&diagnostic.message[0]&&
            !strcmp(diagnostic.message,reasons[n])&&diagnostic.line==locations[n][0]&&
            diagnostic.column==locations[n][1]&&path&&strstr(path,"root.xr");
        if(!semantic){++failures;fprintf(stderr,"capacity rejected%u %s independent semantic oracle FAILED\n",n,cases[n]);}
        xr_compile_resources_free(path);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(capacity_stats(&context).live_bytes==baseline);xr_compile_resources_release(context.resources);
        instance_compile_zero();CHECK(!runtime_live&&!runtime_bytes);
    }
    CHECK(!failures);
}
#endif // XIR_ARRAY_CAPACITY_REJECTIONS_H
