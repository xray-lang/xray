/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_generics_runtime.c - Complete owned Checked specialization, emission and VM execution
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CONSUMER_KIND 3
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_library_catalog.c"
#include "xir/xxir_emit_c.c"
#include "xir_library_generics_oracle.h"
static void library_generics_run_artifact(XrXirArtifact *read,const char *prefix,const char *c_path) {
    CHECK(xr_xir_compile_artifact_verify(read,NULL)==XR_XIR_OK);
    XrXirArtifact *special=NULL,*lowered=NULL;CHECK(xr_xir_compile_specialize(read,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const char *names[]={"main_i64","main_string","main_bool","main_box"};uint32_t entries[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f)for(unsigned i=0;i<4;++i)if(module->functions[f].name_length==strlen(names[i])&&!memcmp(module->functions[f].name,names[i],strlen(names[i]))){CHECK(module->declarations->functions[f].exported);entries[i]=f;}
    for(unsigned i=0;i<4;++i)CHECK(entries[i]!=UINT32_MAX);
    if(c_path){XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(c_path,"wb");CHECK(file&&fwrite(source.text,1,source.length,file)==source.length&&!fclose(file));xr_xir_compile_c_source_free(&source);
        char header[1024];CHECK(snprintf(header,sizeof(header),"%s.h",c_path)>0);file=fopen(header,"wb");CHECK(file);
        CHECK(fprintf(file,"static const uint32_t %s_oracle_entries[4]={%u,%u,%u,%u};\n",prefix,entries[0],entries[1],entries[2],entries[3])>0&&!fclose(file));}
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);library_generics_program_oracle(program,entries);
}
XR_FUNC void library_generics_execute_owned(XrXirArtifact *owned,const char *prefix,const char *c_path) {
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirCompileContext context=*xr_xir_compile_artifact_context(owned);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    library_generics_run_artifact(owned,prefix,c_path);
    XrXirArtifact *read=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    library_generics_run_artifact(read,NULL,NULL);
}
