/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_timed_source.c - Actual timed methods through owned XIR
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
#include "xir_channel_timed_execution_cases.h"
#include "xir_channel_timed_lifecycle_cases.h"
#include "xir_channel_timed_same_consumer_cases.h"
#include "xir_channel_timed_consumer_faults.h"
#include "xir_channel_timed_consumer_budgets.h"
#include "xir_channel_timed_runtime_faults.h"
typedef struct TimedSourcePipeline { XrXirProgram *program;uint32_t entry;const char *output;XrXirArtifact *lowered;bool keep_lowered;uint32_t endpoints[4]; } TimedSourcePipeline;
static XrXirStatus timed_source_pipeline(const XrXirCompileContext *context,void *opaque) {
    TimedSourcePipeline *capture=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CHANNEL_TIMED_FIXTURES};
    XrXirSourceRequest request={session,XR_CHANNEL_TIMED_FIXTURES "/root.xr",&authority,context,XR_CHANNEL_TIMED_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;XrXirCSource c={0};XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&specialized,NULL);xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL);
    xr_xir_compile_artifact_free(specialized);uint32_t entry=UINT32_MAX,endpoints[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);unsigned operations[2]={0};
        for (uint32_t f=0;f<module->function_count;++f) {
            if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"main",4)) { CHECK(entry==UINT32_MAX);entry=f; }
            for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
                XrXirOp op=module->functions[f].instructions[i].op;
                if (op>=XR_XIR_CHANNEL_SEND_TIMEOUT && op<=XR_XIR_CHANNEL_RECV_TIMEOUT) ++operations[op-XR_XIR_CHANNEL_SEND_TIMEOUT];
            }
        }
        CHECK(entry!=UINT32_MAX && operations[0]>=2 && operations[1]>=2);
        const char *names[]={"newTimedUnit","timedUnitSend","timedUnitRecv","timedClose"};
        for (unsigned n=0;n<4;++n) {
            for (uint32_t f=0;f<module->function_count;++f) if (module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
                CHECK(endpoints[n]==UINT32_MAX && module->declarations->functions[f].exported);endpoints[n]=f;
            }
            CHECK(endpoints[n]!=UINT32_MAX);
        }
        status=xr_xir_compile_emit_c(lowered,"channel_timed",4194304,&c);
    }
    if (status==XR_XIR_OK && capture && capture->output) {
        FILE *file=fopen(capture->output,"wb");CHECK(file && fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst unsigned channel_timed_entry_ordinal = %uu;\nconst unsigned channel_timed_endpoints[4] = {%uu,%uu,%uu,%uu};\n",
            entry,endpoints[0],endpoints[1],endpoints[2],endpoints[3])>0 && fclose(file)==0);
    }
    xr_xir_compile_c_source_free(&c);
    if (status==XR_XIR_OK && capture && capture->keep_lowered) { capture->lowered=lowered;lowered=NULL; }
    else if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_artifact_free(lowered);
    if (status==XR_XIR_OK && capture) { capture->program=program;capture->entry=entry;memcpy(capture->endpoints,endpoints,sizeof(endpoints));program=NULL; }
    xr_xir_compile_program_drop(program);
    if (!capture) CHECK(!runtime_live && !runtime_bytes);
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Blocking Source pipeline status=%u at %u:%d:%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if (argc==2 && !strcmp(argv[1],"--faults")) {
        library_compile_operation_cases("Channel timed whole compiler pipeline",timed_source_pipeline,NULL);library_compile_observer_free();return 0;
    }
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);TimedSourcePipeline capture={0};
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-faults"),host=argc==2 && !strcmp(argv[1],"--host");
    bool same=argc==2 && (!strcmp(argv[1],"--same-consumer") || !strcmp(argv[1],"--same-consumer-faults") || !strcmp(argv[1],"--same-consumer-controls") || !strcmp(argv[1],"--same-consumer-budgets"));
    if (argc==2 && !faults && !host && !same) capture.output=argv[1];CHECK(timed_source_pipeline(&owner.context,&capture)==XR_XIR_OK && capture.program);
    if (faults) timed_source_runtime_faults(capture.program,capture.entry);
    else if (same) {
        if (!strcmp(argv[1],"--same-consumer")) timed_same_consumer_cases(capture.program,capture.endpoints);
        else if (!strcmp(argv[1],"--same-consumer-budgets")) timed_consumer_budget_cases(capture.program,capture.endpoints);
        else { timed_consumer_fault_cases(capture.program,capture.endpoints,!strcmp(argv[1],"--same-consumer-faults"));
            if (!strcmp(argv[1],"--same-consumer-faults")) timed_consumer_budget_cases(capture.program,capture.endpoints); }xr_xir_compile_program_drop(capture.program);CHECK(!runtime_live && !runtime_bytes); }
    else if (host) timed_source_host_execute(capture.program,capture.entry);
    else if (argc==1) { timed_life_cases(capture.program,capture.endpoints);timed_source_execute(capture.program,capture.entry); }
    else xr_xir_compile_program_drop(capture.program);
    XrCompileResourceStats stats=library_compile_stats(&owner.context);
    printf("Timed Source full compiler chain work=%llu allocated=%llu peak=%llu\n",(unsigned long long)stats.work,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes);
    library_compile_owner_drop(&owner);library_compile_observer_free();return 0;
}
