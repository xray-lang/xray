/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_routes_producer.c - Emit authentic native exception route programs
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"native Task producer %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../../xir/xir_go_source_native_compile_owner.h"

int main(int argc,char **argv) {
    CHECK(argc==2);
    const XrXirCompileContext *context=effects_source_owner(67108864,128000000);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_NATIVE_TASK_FIXTURE_DIR};
    XrXirSourceRequest request={session,XR_NATIVE_TASK_FIXTURE_DIR "/native_task_throw.xr",&authority,
        context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source status=%u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && xr_xir_compile_source_failure_empty(&failure));
    xr_xir_compile_source_failure_free(&failure);
    xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirDiagnostic lower_diagnostic={0};
    status=xr_xir_compile_lower(closed,&target, NULL,&lowered,&lower_diagnostic);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"native route Lower: status=%u function=%u instruction=%u reason=%u work=%llu\n",
            status,lower_diagnostic.function,lower_diagnostic.instruction,lower_diagnostic.reason,(unsigned long long)stats.work);
    }
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"fail","sibling","awaiting","busy","escaping","relay","catching","escape_root","indirect","rec","recursive"};
    uint32_t entries[11];
    for (unsigned n=0;n<11;++n) {
        unsigned matches=0;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *body=&module->functions[f];
            if (body->name_length==strlen(names[n]) && !memcmp(body->name,names[n],body->name_length)) {
                entries[n]=f;++matches;
            }
        }
        CHECK(matches==1);
    }
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"native_task",4194304,&c)==XR_XIR_OK);
    uint32_t functions=module->function_count;xr_xir_compile_artifact_free(lowered);
    CHECK(c.text && !strstr(c.text,"({"));char path[2048];
    CHECK(snprintf(path,sizeof(path),"%s/native_task.c",argv[1])>0);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(c.text,1,c.length,file)==c.length && !fclose(file));
    CHECK(snprintf(path,sizeof(path),"%s/native_task.h",argv[1])>0);file=fopen(path,"wb");CHECK(file);
    CHECK(fprintf(file,"XR_DATA const XrXirProgramSpec native_task_program;\n")>0);
    const char *constants[]={"FAIL","SIBLING","AWAITING","BUSY","ESCAPING","RELAY","CATCHING","ESCAPE_ROOT","INDIRECT","REC","RECURSIVE"};
    CHECK(fprintf(file,"enum { ")>0);
    for(unsigned n=0;n<11;++n) CHECK(fprintf(file,"NT_%s=%uu,",constants[n],entries[n])>0);
    CHECK(fprintf(file,"NT_FUNCTIONS=%uu };\n",functions)>0 && !fclose(file));
    printf("NATIVE_TASK_PRODUCER functions=%u fail=%u sibling=%u awaiting=%u busy=%u escaping=%u C11bytes=%zu\n",
        functions,entries[0],entries[1],entries[2],entries[3],entries[4],c.length);
    xr_xir_compile_c_source_free(&c);effects_source_owners_free();return 0;
}
