/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_destructure_source.c - Single evaluation and owned binding groups
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "tuple_destructure_cases.h"
static void execute(XrXirProgram *program,uint32_t entry) {
    destructure_runtime_faults(program,entry);
    XrXirValue escaped[2]={{0}};
    for(uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,destructure_output,&groups};
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&escaped[i])==XR_XIR_CALL_RETURNED && groups==1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(uint32_t i=0;i<2;++i)destructure_retained(&escaped[i]);
    CHECK(!runtime_live && !runtime_bytes);
    puts("VM Tuple local/destructure/once/const/shadow/closure/Checked generic/COW/Atomic/escaped physical0 PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TUPLE_DESTRUCTURE_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_TUPLE_DESTRUCTURE_FIXTURES "/root.xr",&authority,context,
        XR_TUPLE_DESTRUCTURE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"status=%u stage=%u line=%d: %s\n",status,diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
    CHECK(status==XR_XIR_OK && product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"make",4)) {CHECK(entry==UINT32_MAX);entry=f;}
    CHECK(entry!=UINT32_MAX);xr_xir_compile_artifact_free(checked);
    XrXirCSource source={0};CHECK(xr_xir_compile_source_product_emit(product,"tuple_destructure",UINT64_C(1048576),&source)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t tuple_destructure_make=%uu;\n",entry)>0 && !fclose(file));
    }
    xr_xir_compile_c_source_free(&source);
    if(argc==1) {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
        xr_xir_compile_source_product_free(product);execute(program,entry);
    } else xr_xir_compile_source_product_free(product);
    source_program_owners_free();return 0;
}
