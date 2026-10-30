/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lowered_correspondence.c - Exact owned declaration matching across lowering
 */
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_sdk_resource_test.h"
#include "api/xsource_submission.c"

typedef struct Artifacts {XrXirArtifact *closed,*lowered;} Artifacts;
static Artifacts artifacts(const XrXirCompileContext *context,const char *text) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CORRESPONDENCE_ROOT};
    char path[1024];int length=snprintf(path,sizeof(path),"%s/control.xr",XR_CORRESPONDENCE_ROOT);
    CHECK(length>0&&(size_t)length<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={"control.xr",text,strlen(text)};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source%u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&source.checked);
    Artifacts result={0};CHECK(xr_xir_compile_specialize(source.checked,&result.closed,NULL, NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(result.closed,&target,NULL,&result.lowered,NULL)==XR_XIR_OK);return result;
}
static void artifacts_free(Artifacts value) {
    xr_xir_compile_artifact_free(value.lowered);xr_xir_compile_artifact_free(value.closed);
}
static uint32_t function_f(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==1&&module->functions[f].name[0]=='f'){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
int main(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;
    XrCompileResources *owner=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={.resources=owner,.limits=xr_xir_compile_default_limits()};
    Artifacts scalar=artifacts(&context,"var state:i64=1\nexport fn f(x:i64)->i64{return x}\n");
    XrXirInstanceBindingSpec row={"f",1,XR_XIR_INSTANCE_BINDING_FUNCTION,function_f(scalar.closed)};
    SubmissionBindingDraft draft={.rows=&row,.count=1};
    CHECK(submission_lowered_bindings(&draft,scalar.closed,scalar.lowered)==XR_XIR_OK);
    const char *wrong[]={"var state:i64=1\nexport fn f(x:f64)->i64{return 1}\n",
        "var state:i64=1\nexport fn f(x:i64)->f64{return 1.0}\n",
        "var state:f64=1.0\nexport fn f(x:i64)->i64{return x}\n"};
    for(unsigned i=0;i<3;++i) {
        Artifacts changed=artifacts(&context,wrong[i]);
        CHECK(function_f(changed.lowered)==function_f(scalar.closed));
        if(i==2) {
            const XrXirModule *module=xr_xir_compile_artifact_module(scalar.closed);
            CHECK(module->declarations->slot_count==1);
            row=(XrXirInstanceBindingSpec){"state",5,XR_XIR_INSTANCE_BINDING_SLOT,0};
        }
        XrXirInstanceBindingSpec saved=row;
        CHECK(submission_lowered_bindings(&draft,scalar.closed,changed.lowered)==XR_XIR_BAD_TYPE);
        CHECK(!memcmp(&saved,&row,sizeof(row)));artifacts_free(changed);
    }
    artifacts_free(scalar);
    Artifacts callable=artifacts(&context,"var state:i64=1\nexport fn f(x:fn()->i64)->fn()->i64{return x}\n");
    row=(XrXirInstanceBindingSpec){"f",1,XR_XIR_INSTANCE_BINDING_FUNCTION,function_f(callable.closed)};
    XrCompileResourceStats before=sdk_stats(owner);size_t attempts=runtime_attempts;
    CHECK(submission_lowered_bindings(&draft,callable.closed,callable.lowered)==XR_XIR_OK&&runtime_attempts>attempts);
    CHECK(sdk_stats(owner).held_bytes==before.held_bytes);
    runtime_fail_at=runtime_attempts;
    CHECK(submission_lowered_bindings(&draft,callable.closed,callable.lowered)==XR_XIR_OUT_OF_MEMORY);
    runtime_fail_at=SIZE_MAX;CHECK(sdk_stats(owner).held_bytes==before.held_bytes);
    CHECK(submission_lowered_bindings(&draft,callable.closed,callable.lowered)==XR_XIR_OK);
    artifacts_free(callable);xr_compile_resources_release(owner);CHECK(!runtime_live&&!runtime_bytes);
    puts("Owned Closed/Lowered correspondence: wrong parameter/result/slot rejected, exact callable accepted, physical OOM distinct and rollback=0/0 PASS");
    return 0;
}
