/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_source_gates.inc.c - Source publication and budget failures
 */
#include "xir/xxir_compile_memory.h"
typedef struct AssertSourceInput {const char *directory,*path;} AssertSourceInput;
static XrXirStatus assert_source_operation(const XrXirCompileContext *context,void *opaque) {
    const AssertSourceInput *input=opaque;XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if(created!=XR_COMPILE_RESOURCE_OK){CHECK(!session);return xir_compile_resource_status(created);}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->directory};
    XrXirSourceRequest request={session,input->path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
    CHECK(status==XR_XIR_OK?(result.checked&&result.snapshot):(!result.checked&&!result.snapshot));
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return status;
}
static void assert_source_oom(const char *directory,const char *path) {
    assert_write(path,"export fn run()->i64 {try{assert(false)}catch panic(p){return p.code};return 0}\n");
    AssertSourceInput input={directory,path};library_compile_operation_cases("Assertion whole Source/session/Core",assert_source_operation,&input);
    CHECK(!assert_compile_extra_blocks()&&!assert_compile_extra_bytes()&&!runtime_live&&!runtime_bytes);
}
