/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_a2_source.c - Independent Atomic execution and ownership qualification
 *
 * KEY CONCEPT:
 *   Each backend checks fixed expectations after the ordinary source pipeline.
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
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_atomic_a2_execution_cases.h"
typedef struct AtomicPipeline {XrXirProgram *program;uint32_t entry;const char *output;} AtomicPipeline;
static XrXirStatus atomic_source_pipeline(const XrXirCompileContext *context,void *opaque) {
    AtomicPipeline *capture=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus cs=xr_compile_session_new(context->resources,&session);
    if(cs!=XR_COMPILER_SESSION_OK)return cs==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ATOMIC_A2_FIXTURES};
    XrXirSourceRequest request={session,XR_ATOMIC_A2_FIXTURES "/root.xr",&authority,context,XR_ATOMIC_A2_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;XrXirCSource c={0};XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(read,&specialized,NULL);
    xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);
    xr_xir_compile_artifact_free(specialized);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    uint32_t entry=UINT32_MAX;
    if(status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->declarations->functions[f].module==module->declarations->root_module &&
                module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"main",4)) {
                CHECK(entry==UINT32_MAX && module->declarations->functions[f].exported==1);entry=f;
            }
        CHECK(entry!=UINT32_MAX);
    }
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_c(lowered,"atomic_a2",4194304,&c);
    if(status==XR_XIR_OK && capture && capture->output) {
        FILE *file=fopen(capture->output,"wb");CHECK(file);
        CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst unsigned atomic_a2_entry = %uu;\n",entry)>0 && fclose(file)==0);
    }
    xr_xir_compile_c_source_free(&c);
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_artifact_free(lowered);
    if(status==XR_XIR_OK && capture) {capture->program=program;capture->entry=entry;program=NULL;}
    xr_xir_compile_program_drop(program);
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Atomic Source pipeline status %u at %u:%d:%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if(argc==2 && !strcmp(argv[1],"--faults")) {
        library_compile_operation_cases("Atomic Source to native C and VM program",atomic_source_pipeline,NULL);
        library_compile_observer_free();
        puts("Atomic whole compiler pipeline fault ordinals, three axes and physical release passed");
        return 0;
    }
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    AtomicPipeline capture={0};if(argc==2)capture.output=argv[1];
    CHECK(atomic_source_pipeline(&owner.context,&capture)==XR_XIR_OK && capture.program);
    if(argc==1)atomic_execute_pair(capture.program,capture.entry);
    else xr_xir_compile_program_drop(capture.program);
    library_compile_owner_drop(&owner);
    library_compile_observer_free();
    puts(argc==1 ? "Atomic three-module Source/Checked replay and Lowered VM independent output passed" :
        "Atomic three-module Checked replay and native C generation passed; VM execution not run");
    return 0;
}
