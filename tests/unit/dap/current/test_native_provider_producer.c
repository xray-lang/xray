/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_provider_producer.c - Emit the canonical SourceCompiler native consumer
 */
#include "api/xsource_compiler.h"
#include "program/xr_xir_source_product.h"
#include "xir/xxir_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"native provider producer %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../../xir/xir_go_source_native_compile_owner.h"
int main(int argc,char **argv) {
    CHECK(argc==2);
    const XrXirCompileContext *context=effects_source_owner(67108864,128000000);
    XrSourceCompilerConfig config={*context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_SOURCE_STDLIB,
        xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *source=NULL;
    XrSourceCompileStatus status=xr_source_compiler_build_file(compiler,XR_NATIVE_PROVIDER_FIXTURE,&source,&diagnostic);
    if(status!=XR_SOURCE_COMPILE_OK) { char *message=NULL;(void)xr_frontend_report_render(diagnostic.source.failure.report,&message);fprintf(stderr,"%s\n",message?message:"diagnostic unavailable");xr_compile_resources_free(message); }
    CHECK(status==XR_SOURCE_COMPILE_OK);
    XrXirSourceDiagnostic immediate={0};CHECK(xr_xir_compile_source_product_public_admit(source,&immediate)==XR_XIR_OK);
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(source);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_source_product_packet(source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read_lower(context,packet.bytes,packet.length,&facts->target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t selected[2]={UINT32_MAX,UINT32_MAX};const char *names[]={"native_leaf","native_caller"};
    for(unsigned n=0;n<2;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
            CHECK(selected[n]==UINT32_MAX);selected[n]=f;
        }
    CHECK(selected[0]!=UINT32_MAX && selected[1]!=UINT32_MAX);
    uint32_t count=facts->function_count,entry=facts->entry;
    XrXirCSource c={0};CHECK(xr_xir_compile_source_product_emit(source,"native_provider",4194304,&c)==XR_XIR_OK);
    CHECK(c.text && !strstr(c.text,"({"));
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_source_product_free(source);
    xr_source_compile_diagnostic_free(&diagnostic);xr_source_compiler_free(compiler);
    char file_path[2048];CHECK(snprintf(file_path,sizeof(file_path),"%s/native_provider.c",argv[1])>0);
    FILE *file=fopen(file_path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length && !fclose(file));
    CHECK(snprintf(file_path,sizeof(file_path),"%s/native_provider.h",argv[1])>0);file=fopen(file_path,"wb");CHECK(file);
    CHECK(fprintf(file,"XR_DATA const XrXirProgramSpec native_provider_program;\nXR_DATA const XirCallNativeSourceCode native_provider_native_source;\n")>0);
    CHECK(fprintf(file,"enum { NP_ENTRY=%uu,NP_LEAF=%uu,NP_CALLER=%uu,NP_FUNCTIONS=%uu };\n",entry,selected[0],selected[1],count)>0 && !fclose(file));
    printf("NATIVE_PROVIDER_PRODUCER functions=%u entry=%u leaf=%u caller=%u C11bytes=%zu\n",count,entry,selected[0],selected[1],c.length);
    xr_xir_compile_c_source_free(&c);effects_source_owners_free();return 0;
}
