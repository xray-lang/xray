/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_inline_source.c - Owned source Checked execution after producer destruction
 *
 * KEY CONCEPT:
 *   The first owned Checked result survives the source snapshot/session and executes directly.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_class_pipeline_owner.h"
#include "xir/xxir_vm.h"
#define SOURCE_CLASS_FUNCTIONS 19
#define LOWERED_CLASS_FUNCTIONS 25
XR_FUNC void xr_test_class_inline_source_run(const XrXirCompileContext *context,XrXirArtifact **checked);
XR_FUNC size_t xr_test_class_inline_runtime_live(void);
XR_FUNC size_t xr_test_class_inline_runtime_bytes(void);

static XrXirStatus class_source_owned(const XrXirCompileContext *context,XrXirArtifact **output) {
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    if(status==XR_XIR_OK){
        status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        if(status!=XR_XIR_OK){CHECK(diagnostic.status==status && !result.checked);
            if(result.snapshot)CHECK(!xr_xir_compile_source_snapshot_view(result.snapshot)->complete);}
    }
    if(status==XR_XIR_OK){
        CHECK(result.checked && result.snapshot && xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        CHECK(xr_xir_compile_artifact_module(result.checked)->function_count==SOURCE_CLASS_FUNCTIONS);
        *output=result.checked;result.checked=NULL;
    }
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(*output,NULL);
    if(status!=XR_XIR_OK){xr_xir_compile_artifact_free(*output);*output=NULL;}
    return status;
}
static XrXirStatus class_source_program(const XrXirCompileContext *context,XrXirProgram **output,const char *path) {
    XrXirArtifact *owned=NULL,*decoded=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=class_source_owned(context,&owned);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(owned,&packet,NULL);
    if(status==XR_XIR_OK){CHECK(packet.length<=262144);if(path){FILE *f=fopen(path,"wb");CHECK(f && fwrite(packet.bytes,1,packet.length,f)==packet.length && !fclose(f));}}
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(owned);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(decoded,&special,NULL);
    xr_xir_compile_artifact_free(decoded);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    xr_xir_compile_artifact_free(special);
    if(status==XR_XIR_OK){const XrXirModule *m=xr_xir_compile_artifact_module(lowered);
        CHECK(m->function_count==LOWERED_CLASS_FUNCTIONS && m->declarations->module_count==3);
        CHECK(m->functions[3].name_length==6 && !memcmp(m->functions[3].name,"answer",6));
        CHECK(m->functions[4].name_length==8 && !memcmp(m->functions[4].name,"retained",8));
        status=xr_xir_compile_vm_program_take(&lowered,output);
    }
    xr_xir_compile_artifact_free(lowered);if(status!=XR_XIR_OK)CHECK(!*output);return status;
}
static XrXirStatus class_source_operation(const XrXirCompileContext *context,void *fixture) {
    (void)fixture;XrXirProgram *program=NULL;XrXirStatus status=class_source_program(context,&program,NULL);
    xr_xir_compile_program_drop(program);return status;
}

int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);ClassPipelineOwner owner={0};CHECK(class_pipeline_new(&owner,&class_pipeline_limits)==XR_XIR_OK);
    XrXirArtifact *owned=NULL;CHECK(class_source_owned(&owner.context,&owned)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK && packet.length<=262144);
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *decoded=NULL;CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    /* The original first Checked owner is retained while independent complete fault replays destroy all their producers. */
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    class_pipeline_faults("Class inline whole Source/Checked/VM",class_source_operation,NULL);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    xr_test_class_inline_source_run(&owner.context,&owned);CHECK(!owned);
    CHECK(!xr_test_class_inline_runtime_live() && !xr_test_class_inline_runtime_bytes());
    class_pipeline_drop(&owner);class_pipeline_final_zero();puts("inline class first OwnedChecked; complete compiler FI/axes; both physical0 PASS");return 0;
}
