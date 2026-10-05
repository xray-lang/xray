/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_shadow_rejections.h - Ordinary function authority failures
 */
static void shadow_rejections(void) {
    static const char *const names[]={"value_type","type_arity","ref_permission","declaration_constraint",
        "unused_default","value_shadow","private_import","ref_fact","unused_body"};
    static const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_VALUE,XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE};
    _Static_assert(sizeof(names)/sizeof(names[0])==sizeof(expected)/sizeof(expected[0]),"Independent status for every source");
    for (unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n) {
        instance_compile_zero();instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        const XrCompileResourceLimits limits=remove_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        const uint64_t baseline=remove_stats(&context).live_bytes;
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        char input[1024];CHECK(snprintf(input,sizeof(input),"%s/shadow_rejected/%s/root.xr",XR_REMOVE_FIXTURES,names[n])>0);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REMOVE_FIXTURES};
        XrXirSourceRequest request={session,input,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
        const XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
        printf("shadow negative%u %s status%u line%d:%d %s\n",n,names[n],status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==expected[n] && diagnostic.status==status && !result.checked && !result.snapshot);
        xr_compile_resources_free(path);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(remove_stats(&context).live_bytes==baseline);xr_compile_resources_release(context.resources);
        instance_compile_zero();CHECK(!runtime_live && !runtime_bytes);
    }
}
