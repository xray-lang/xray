/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_cycle_batch_producer.c - Source single and branching closure rings
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Cycle batch producer %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_effect_execution_owner.h"
int main(int argc,char **argv) {
    CHECK(argc==2);const XrXirCompileContext context=*effects_source_owner(67108864,128000000);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CYCLE_BATCH_FIXTURE_ROOT};
    XrXirSourceRequest request={session,XR_CYCLE_BATCH_FIXTURE_ROOT "/xir_p20_cycle_batch/root.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Cycle batch Source status=%u at=%d:%d message=%s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xA5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const char *names[]={"run","make","make_branch"};uint32_t entries[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for(unsigned i=0;i<3;++i){for(uint32_t f=0;f<module->function_count;++f){const XrXirFunction *function=&module->functions[f];if(function->name_length!=strlen(names[i]) || memcmp(function->name,names[i],function->name_length))continue;CHECK(entries[i]==UINT32_MAX && function->parameter_count==(i?1u:2u));CHECK((module->declarations->functions[f].module==module->declarations->root_module)==(i==0));entries[i]=f;}CHECK(entries[i]!=UINT32_MAX);}
    XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,"p20_cycle_batch_fixture",4194304,&generated)==XR_XIR_OK);
    CHECK(generated.text && !strstr(generated.text,"({") && !strstr(generated.text,"xr_xir_vm"));
    FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(generated.text,1,generated.length,file)==generated.length);
    CHECK(fprintf(file,"\nconst uint32_t p20_cycle_batch_entries[3] = {%uu,%uu,%uu};\nconst uint32_t p20_cycle_batch_pair_entries[2] = {%uu,%uu};\n",entries[0],entries[1],entries[2],entries[0],entries[1])>0 && fclose(file)==0);
    printf("Cycle batch Source modules=%u functions=%u slots=%u Cbytes=%zu captures1and2 original128M helper4M\n",module->declarations->module_count,module->function_count,module->declarations->slot_count,generated.length);
    xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);effects_source_owners_free();return 0;
}
