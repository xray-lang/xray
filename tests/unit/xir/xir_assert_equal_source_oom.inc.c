/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_equal_source_oom.inc.c - Source, binder and default failure publication
 */
typedef struct AssertSourceInput {const char *directory,*path;} AssertSourceInput;
static XrXirStatus assert_source_operation(const XrXirCompileContext *context,void *opaque) {
    const AssertSourceInput *input=opaque;XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if(created!=XR_COMPILER_SESSION_OK){CHECK(!session);return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->directory};
    XrXirSourceRequest request={session,input->path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
    CHECK(status==XR_XIR_OK?(result.checked&&result.snapshot):(!result.checked&&!result.snapshot));
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return status;
}
static void equal_source_oom(const char *directory,const char *path,const char *source) {
    panics_write(path,source);
    AssertSourceInput input={directory,path};library_compile_operation_cases("Assertion whole Source/session/Core",assert_source_operation,&input);
    CHECK(!assert_compile_extra_blocks()&&!assert_compile_extra_bytes()&&!runtime_live&&!runtime_bytes);
}
