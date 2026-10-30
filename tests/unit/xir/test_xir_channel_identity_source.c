/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_identity_source.c - Real Source Channel identities in VM and generated C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_channel_identity_execution_cases.h"
#include "xir_channel_identity_ir_cases.h"
typedef struct ChannelIdentityPipeline { XrXirProgram *program;uint32_t entries[3];const char *output; } ChannelIdentityPipeline;
static XrXirStatus channel_identity_pipeline(const XrXirCompileContext *context,void *opaque) {
    ChannelIdentityPipeline *capture=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CHANNEL_IDENTITY_FIXTURES};
    XrXirSourceRequest request={session,XR_CHANNEL_IDENTITY_FIXTURES "/root.xr",&authority,context,XR_CHANNEL_IDENTITY_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;XrXirCSource c={0};XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&specialized,NULL, NULL);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL);
    xr_xir_compile_artifact_free(specialized);
    uint32_t entries[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};const char *names[]={"main","negative","oversized"};
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        for (uint32_t f=0;f<module->function_count;++f) for (unsigned name=0;name<3;++name)
            if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==strlen(names[name]) && !memcmp(module->functions[f].name,names[name],strlen(names[name]))) {
                CHECK(entries[name]==UINT32_MAX && module->declarations->functions[f].exported==1);entries[name]=f;
            }
        CHECK(entries[0]!=UINT32_MAX && entries[1]!=UINT32_MAX && entries[2]!=UINT32_MAX);
        status=xr_xir_compile_emit_c(lowered,"channel_identity",4194304,&c);
    }
    if (status==XR_XIR_OK && capture && capture->output) {
        FILE *file=fopen(capture->output,"wb");CHECK(file && fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst unsigned channel_identity_entry_ordinals[3] = {%uu,%uu,%uu};\n",entries[0],entries[1],entries[2])>0 && fclose(file)==0);
    }
    xr_xir_compile_c_source_free(&c);
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_artifact_free(lowered);
    if (status==XR_XIR_OK && capture) { capture->program=program;memcpy(capture->entries,entries,sizeof(entries));program=NULL; }
    xr_xir_compile_program_drop(program);
    if (!capture) CHECK(!runtime_live && !runtime_bytes);
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Channel Source pipeline status=%u at %u:%d:%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if (argc==2 && !strcmp(argv[1],"--faults")) {
        library_compile_operation_cases("Channel identity whole compiler pipeline",channel_identity_pipeline,NULL);
        library_compile_observer_free();return 0;
    }
    bool runtime_faults=argc==2 && !strcmp(argv[1],"--runtime-faults");
    channel_identity_ir_cases();
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    ChannelIdentityPipeline capture={0};if (argc==2 && !runtime_faults) capture.output=argv[1];
    CHECK(channel_identity_pipeline(&owner.context,&capture)==XR_XIR_OK && capture.program);
    if (runtime_faults) channel_identity_runtime_faults(capture.program,capture.entries[0]);
    else if (argc==1) channel_identity_execute(capture.program,capture.entries);else xr_xir_compile_program_drop(capture.program);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts(runtime_faults ? "Channel identity VM runtime fault qualification PASS" :
        argc==1 ? "Channel identity Source/Checked/VM exact typed outputs and two instances PASS" : "Channel identity actual native C generated; execution not run");
    return 0;
}
