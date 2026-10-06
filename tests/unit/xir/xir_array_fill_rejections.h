/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fill_rejections.h - Real invalid Source requests retain no partial output
 *
 * KEY CONCEPT:
 *   Ordinary generic declarations never gain access from a future instantiation.
 */
#ifndef XIR_ARRAY_FILL_REJECTIONS_H
#define XIR_ARRAY_FILL_REJECTIONS_H
static void fill_rejections(void) {
    static const char *const cases[]={"const","read","temporary","arity0","arity4","typearg","ref_value","bad_value","bad_start","bad_end","flatten","const_field","private_field","generic"};
    for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);++n){
        instance_compile_zero();instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};const XrCompileResourceLimits limits=fill_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline=fill_stats(&context).live_bytes;
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        char name[1024];CHECK(snprintf(name,sizeof(name),"%s/rejected/%s/root.xr",XR_FILL_FIXTURES,cases[n])>0);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FILL_FIXTURES};
        XrXirSourceRequest request={session,name,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
        printf("fill rejected%u %s status%u line%d:%d reason=%s\n",n,cases[n],status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE&&diagnostic.status==status&&!result.checked&&!result.snapshot);
        CHECK(*diagnostic.message);
        xr_compile_resources_free(path);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(fill_stats(&context).live_bytes==baseline);xr_compile_resources_release(context.resources);
        instance_compile_zero();CHECK(!runtime_live&&!runtime_bytes);
    }
}
#endif
