/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_capacity_native_producer.c - Complete bounded native capacity producer
 *
 * KEY CONCEPT:
 *   The emitted program survives destruction of all compiler stage owners.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_instance_compile_observer.h"
#include "xir_source_fixture_owner.h"
#include "xir_array_capacity_native_oracles.h"
#include "xir_array_capacity_native_path_pipeline.h"
static uint32_t capacity_native_find(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f){
        if(module->declarations->functions[f].module!=module->declarations->root_module)continue;
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length==length&&!memcmp(function->name,name,length)){
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
int main(int argc,char **argv) {
    CHECK(argc==4);instance_compile_zero();SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CAP_NATIVE_FIXTURES};
    XrXirSourceRequest request={session,XR_CAP_NATIVE_FIXTURES "/root.xr",&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&path);
    if(status!=XR_XIR_OK)fprintf(stderr,"native capacity Source status%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0};XrXirArtifact *replay=NULL,*specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&replay,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_compile_resources_free(path);
    CHECK(xr_xir_compile_specialize(replay,&specialized,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(replay);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    FILE *header=fopen(argv[2],"wb");CHECK(header);
    CHECK(fputs("XR_DATA const XrXirProgramSpec capacity_native_contract_program;\n"
        "XR_DATA const XrXirProgramSpec capacity_native_path_program;\n"
        "static const uint32_t capacity_native_entries[10]={",header)>=0);
    for(unsigned i=0;i<CAP_NATIVE_COUNT;++i)
        CHECK(fprintf(header,"%s%uu",i?",":"",capacity_native_find(module,capacity_native_names[i]))>0);
    CHECK(fputs("};\n",header)>=0&&!fclose(header));
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"capacity_native_contract",1048576,&c)==XR_XIR_OK);
    CHECK(c.text&&c.length&&!c.text[c.length]&&!strstr(c.text,"({"));
    char observed[4096];CHECK(snprintf(observed,sizeof(observed),"%s.helper-observation.c",argv[1])>0);
    FILE *observation=fopen(observed,"wb");CHECK(observation);
    CHECK(fwrite(c.text,1,c.length,observation)==c.length&&!fclose(observation));
    const char *const helper_names[]={"xr_xir_instance_array_capacity(","xr_xir_instance_path_capacity(",
        "xr_xir_instance_array_with_capacity(","xr_xir_instance_array_reserve("};
    for(unsigned i=0;i<4;++i)fprintf(stderr,"NATIVE_CAPACITY_HELPER %s present=%u Cbytes=%zu\n",
        helper_names[i],strstr(c.text,helper_names[i])?1u:0u,c.length);
    CHECK(strstr(c.text,"xr_xir_instance_array_capacity(")&&
        strstr(c.text,"xr_xir_instance_array_with_capacity(")&&strstr(c.text,"xr_xir_instance_array_reserve("));
    xr_xir_compile_artifact_free(lowered);
    FILE *file=fopen(argv[1],"wb");CHECK(file);
    CHECK(fwrite(c.text,1,c.length,file)==c.length&&!fclose(file));
    printf("NATIVE_CAPACITY_GENERATED completeSourceCheckedReplaySpecializeLowered=1 C11bytes=%zu Sourcehelpers=3 GNU=0\n",c.length);
    xr_xir_compile_c_source_free(&c);capacity_native_emit_path(&owner.context,argv[3]);
    source_fixture_owner_free(&owner);instance_compile_report();return 0;
}
