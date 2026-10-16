/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_try_source.c - Actual Source nonblocking Channel operations
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
#include "xir_channel_try_execution_cases.h"
#include "xir_channel_retry_execution_cases.h"
#include "xir_channel_rendezvous_backend_cases.h"
#include "xir_channel_try_ir_cases.h"
typedef struct ChannelTryPipeline { XrXirProgram *program;uint32_t entry,retries[4],rendezvous[3];const char *output;
    XrXirArtifact *lowered;bool keep_lowered; } ChannelTryPipeline;
static XrXirStatus channel_try_pipeline(const XrXirCompileContext *context,void *opaque) {
    ChannelTryPipeline *capture=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CHANNEL_TRY_FIXTURES};
    XrXirSourceRequest request={session,XR_CHANNEL_TRY_FIXTURES "/root.xr",&authority,context,XR_CHANNEL_TRY_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;XrXirCSource c={0};XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&specialized,NULL);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);
    xr_xir_compile_artifact_free(specialized);uint32_t entry=UINT32_MAX,retries[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    uint32_t rendezvous[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        unsigned unit_reads=0;
        for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i)
            if (module->functions[f].instructions[i].op==XR_XIR_ENUM_GET && module->functions[f].instructions[i].type==XR_XIR_UNIT) ++unit_reads;
        CHECK(unit_reads==3);
        for (uint32_t f=0;f<module->function_count;++f) if (module->declarations->functions[f].module==module->declarations->root_module &&
            module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"main",4)) {
            CHECK(entry==UINT32_MAX && module->declarations->functions[f].exported==1);entry=f;
        }
        CHECK(entry!=UINT32_MAX);status=xr_xir_compile_emit_c(lowered,"channel_try",4194304,&c);
        const char *names[]={"newRetryChannel","retrySend","retryRecv","retryClose"};
        for (unsigned n=0;n<4;++n) {
            for (uint32_t f=0;f<module->function_count;++f) if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
                CHECK(retries[n]==UINT32_MAX && module->declarations->functions[f].exported==1);retries[n]=f;
            }
            CHECK(retries[n]!=UINT32_MAX);
        }
        const char *rendezvous_names[]={"newRendezvousChannel","rendezvousRecv","rendezvousSend"};
        for (unsigned n=0;n<3;++n) {
            for (uint32_t f=0;f<module->function_count;++f) if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==strlen(rendezvous_names[n]) && !memcmp(module->functions[f].name,rendezvous_names[n],strlen(rendezvous_names[n]))) {
                CHECK(rendezvous[n]==UINT32_MAX && module->declarations->functions[f].exported==1);rendezvous[n]=f;
            }
            CHECK(rendezvous[n]!=UINT32_MAX);
        }
    }
    if (status==XR_XIR_OK && capture && capture->output) {
        FILE *file=fopen(capture->output,"wb");CHECK(file && fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst unsigned channel_try_entry_ordinal = %uu;\nconst unsigned channel_try_retry_ordinals[4] = {%uu,%uu,%uu,%uu};\nconst unsigned channel_try_rendezvous_ordinals[3] = {%uu,%uu,%uu};\n",
            entry,retries[0],retries[1],retries[2],retries[3],rendezvous[0],rendezvous[1],rendezvous[2])>0 && fclose(file)==0);
    }
    xr_xir_compile_c_source_free(&c);
    if (status==XR_XIR_OK && capture && capture->keep_lowered) { capture->lowered=lowered;lowered=NULL; }
    else if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_artifact_free(lowered);
    if (status==XR_XIR_OK && capture) {
        capture->program=program;capture->entry=entry;memcpy(capture->retries,retries,sizeof(retries));
        memcpy(capture->rendezvous,rendezvous,sizeof(rendezvous));program=NULL;
    }
    xr_xir_compile_program_drop(program);if (!capture) CHECK(!runtime_live && !runtime_bytes);
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Channel try pipeline status=%u at %u:%d:%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if (argc==2 && !strcmp(argv[1],"--faults")) {
        library_compile_operation_cases("Channel try whole compiler pipeline",channel_try_pipeline,NULL);library_compile_observer_free();return 0;
    }
    channel_try_ir_cases();
    channel_unit_construct_ir_cases();
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    bool runtime_faults=argc==2 && !strcmp(argv[1],"--runtime-faults");
    ChannelTryPipeline capture={0};if (argc==2 && !runtime_faults) capture.output=argv[1];
    CHECK(channel_try_pipeline(&owner.context,&capture)==XR_XIR_OK && capture.program);
    if (runtime_faults) { rendezvous_backend_faults(capture.program,capture.rendezvous);channel_try_runtime_faults(capture.program,capture.entry); }
    else if (argc==1) { channel_retry_execute(capture.program,capture.retries);rendezvous_backend_execute(capture.program,capture.rendezvous);channel_try_execute(capture.program,capture.entry); }
    else xr_xir_compile_program_drop(capture.program);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts(runtime_faults ? "Channel try VM runtime fault qualification PASS" :
        argc==1 ? "Channel try actual Source/Checked/VM execution PASS" : "Channel try real native C generated; execution not run");return 0;
}
