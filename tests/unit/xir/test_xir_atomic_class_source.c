/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_class_source.c - Class-held Atomic values cross the owned compiler pipeline
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
#include "xir_runtime_allocations.h"
#include "xir_atomic_class_execution.h"
typedef struct AtomicClassInput {
    const char *directory,*stdlib,*output;
    XrXirProgram *program;
    uint32_t entries[2];
} AtomicClassInput;
static XrXirStatus atomic_class_pipeline(const XrXirCompileContext *context,void *opaque) {
    AtomicClassInput *input=opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    if (made!=XR_COMPILER_SESSION_OK) return made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char path[2048];int length=snprintf(path,sizeof(path),"%s/root.xr",input->directory);
    CHECK(length>0 && (size_t)length<sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->directory};
    XrXirSourceRequest request={session,path,&authority,context,input->stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *read=NULL,*special=NULL,*lowered=NULL;XrXirCSource emitted={0};XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&special,NULL);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(special,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);
    xr_xir_compile_artifact_free(special);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        const char *names[]={"main","retained"};
        for (unsigned n=0;n<2;++n) {
            input->entries[n]=UINT32_MAX;
            for (uint32_t f=0;f<module->function_count;++f)
                if (module->declarations->functions[f].exported && module->functions[f].name_length==strlen(names[n]) &&
                    !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
                    CHECK(input->entries[n]==UINT32_MAX);input->entries[n]=f;
                }
            CHECK(input->entries[n]!=UINT32_MAX);
        }
        status=xr_xir_compile_emit_c(lowered,"atomic_class",4194304,&emitted);
    }
    if (status==XR_XIR_OK && input->output) {
        CHECK(!strstr(emitted.text,"({"));FILE *file=fopen(input->output,"wb");CHECK(file);
        CHECK(fwrite(emitted.text,1,emitted.length,file)==emitted.length);
        CHECK(fprintf(file,"\nconst uint32_t atomic_class_ids[2]={%u,%u};\n",input->entries[0],input->entries[1])>0 && !fclose(file));
    }
    xr_xir_compile_c_source_free(&emitted);
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_artifact_free(lowered);
    if (status==XR_XIR_OK && !input->output) {input->program=program;program=NULL;}
    xr_xir_compile_program_drop(program);
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Atomic Class status=%u module=%u line=%d column=%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
static XrXirStatus atomic_class_compile_operation(const XrXirCompileContext *context,void *opaque) {
    AtomicClassInput input=*(AtomicClassInput *)opaque;input.program=NULL;input.output=NULL;
    XrXirStatus status=atomic_class_pipeline(context,&input);xr_xir_compile_program_drop(input.program);return status;
}
static void atomic_class_same_owner(AtomicClassInput *input) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;
    CHECK(atomic_class_compile_operation(&owner.context,input)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;CHECK(sites);library_compile_owner_drop(&owner);
    for (size_t site=0;site<sites;++site) {
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=atomic_class_compile_operation(&owner.context,input);source_program_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        XrCompileResourceStats failed=library_compile_stats(&owner.context);
        CHECK(failed.live_bytes==owner.baseline.live_bytes);
        CHECK(atomic_class_compile_operation(&owner.context,input)==XR_XIR_OK);
        XrCompileResourceStats recovered=library_compile_stats(&owner.context);
        CHECK(recovered.live_bytes==failed.live_bytes && recovered.work>failed.work &&
            recovered.allocated_bytes>failed.allocated_bytes);
        library_compile_owner_drop(&owner);
    }
    printf("Atomic Class same-owner fault/retry pairs=%zu without ledger reset PASS\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==3 || argc==4);AtomicClassInput input={.directory=argv[1],.stdlib=argv[2]};
    if (argc==4 && !strcmp(argv[3],"--faults")) {
        library_compile_operation_cases("Atomic Class whole Source/Checked/CGen/VM",atomic_class_compile_operation,&input);
        library_compile_observer_free();return 0;
    }
    if (argc==4 && !strcmp(argv[3],"--retry")) {
        atomic_class_same_owner(&input);library_compile_observer_free();return 0;
    }
    if (argc==4 && strcmp(argv[3],"--expect-reject")) input.output=argv[3];
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirStatus status=atomic_class_pipeline(&owner.context,&input);
    if (argc==4 && !strcmp(argv[3],"--expect-reject")) CHECK(status==XR_XIR_BAD_TYPE && !input.program);
    else {
        CHECK(status==XR_XIR_OK);
        if (!input.output) {CHECK(input.program);atomic_class_execute(input.program,input.entries);}
    }
    library_compile_owner_drop(&owner);library_compile_observer_free();CHECK(!runtime_live && !runtime_bytes);
    puts("Atomic Class pipeline and physical cleanup completed");return 0;
}
