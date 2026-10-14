/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_transfer.c - Real product and Program ownership handoff failures
 */
#include "api/xsource_api.h"
#include "api/xsource_compiler.h"
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"
typedef struct SourceTransfer {
    XrSourceCompiler *compiler;
    XrXirSourceProduct *product;
    XrSourceProgram *program;
    XrXirProgram *code;
    uint32_t entry;
} SourceTransfer;
static XrSourceStatus transfer(SourceTransfer *owned,const XrCompileResourceLimits *limits,
    XrCompileResourceStats *stats) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owned->compiler);if(status!=XR_SOURCE_OK)return status;
    XrCompileResources *resources=xr_source_compiler_resources(owned->compiler);
    const char text[]="const n:i64 = 42\n";
    XrSourceCompilerText input={NULL,"real-product-transfer",text,sizeof(text)-1};
    status=api_compile(xr_source_compiler_build_text(owned->compiler,&input,&owned->product,NULL));if(status!=XR_SOURCE_OK)return status;
    XrXirSourceProduct *original=owned->product;status=xr_source_program_take_product(&owned->product,&owned->program);
    if(status!=XR_SOURCE_OK){CHECK(owned->product==original && !owned->program);return status;}
    CHECK(!owned->product && owned->program);xray_source_compiler_free(owned->compiler);owned->compiler=NULL;
    XrSourceProgram *wrapper=owned->program;status=xr_source_program_take_vm(&owned->program,&owned->code,&owned->entry);
    if(status!=XR_SOURCE_OK)CHECK(owned->program==wrapper && !owned->code && owned->entry==UINT32_MAX);
    else {
        CHECK(!owned->program && owned->code && owned->entry!=UINT32_MAX);
        if(stats)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    }
    return status;
}
static void transfer_close(SourceTransfer *owned) {
    xray_source_compiler_free(owned->compiler);xr_xir_compile_source_product_free(owned->product);
    xray_source_program_free(owned->program);xr_xir_compile_program_drop(owned->code);
    CHECK(!runtime_live && !runtime_bytes);
}
static void transfer_budgets(void) {
    runtime_fail_at=SIZE_MAX;
    XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    XrCompileResourceStats required={0};SourceTransfer owned={0};owned.entry=UINT32_MAX;
    CHECK(transfer(&owned,&generous,&required)==XR_SOURCE_OK);transfer_close(&owned);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        owned=(SourceTransfer){0};owned.entry=UINT32_MAX;
        XrSourceStatus status=transfer(&owned,&limits,NULL);
        CHECK(status==(delta<0?XR_SOURCE_BUDGET:XR_SOURCE_OK));
        if(delta<0)CHECK(!owned.code && owned.entry==UINT32_MAX);
        transfer_close(&owned);
    }
    puts("actual transfer resource axes at exact/minus-one/plus-one preserve output; physical=0/0");
}
static void transfer_guards(void) {
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    const char text[]="const n:i64 = 42\n";
    XrSourceCompilerText input={NULL,"transfer-guards",text,sizeof(text)-1};
    XrXirSourceProduct *first=NULL,*second=NULL;
    CHECK(xr_source_compiler_build_text(compiler,&input,&first,NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_source_compiler_build_text(compiler,&input,&second,NULL)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *saved_first=first,*saved_second=second;
    XrSourceProgram *program=NULL,*other=NULL;
    size_t before=runtime_attempts;
    CHECK(xr_source_program_take_product(NULL,&program)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xr_source_program_take_product(&first,NULL)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(first==saved_first && !program && runtime_attempts==before);
    CHECK(xr_source_program_take_product(&first,&program)==XR_SOURCE_OK && !first && program);
    XrSourceProgram *saved_program=program;before=runtime_attempts;
    CHECK(xr_source_program_take_product(&second,&program)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(second==saved_second && program==saved_program && runtime_attempts==before);
    CHECK(xr_source_program_take_product(&second,&other)==XR_SOURCE_OK && !second && other);
    xray_source_compiler_free(compiler);
    XrXirProgram *code=NULL;uint32_t entry=UINT32_MAX;before=runtime_attempts;
    CHECK(xr_source_program_take_vm(NULL,&code,&entry)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xr_source_program_take_vm(&program,NULL,&entry)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xr_source_program_take_vm(&program,&code,NULL)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(program==saved_program && !code && entry==UINT32_MAX && runtime_attempts==before);
    CHECK(xr_source_program_take_vm(&program,&code,&entry)==XR_SOURCE_OK && !program && code);
    XrXirProgram *saved_code=code;XrSourceProgram *saved_other=other;
    entry=UINT32_MAX;before=runtime_attempts;
    CHECK(xr_source_program_take_vm(&other,&code,&entry)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xr_source_program_take_vm(&program,&code,&entry)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(other==saved_other && code==saved_code && entry==UINT32_MAX && runtime_attempts==before);
    xr_xir_compile_program_drop(code);xray_source_program_free(other);
    CHECK(!runtime_live && !runtime_bytes);
    puts("actual transfer guards preserve owners, occupied outputs and entry without allocation; physical=0/0");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--guards")));
    transfer_guards();transfer_budgets();
    if(argc==2)return 0;
    size_t count=0;
    for(size_t point=0;!point || point<=count;++point) {
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        SourceTransfer owned={0};owned.entry=UINT32_MAX;
        XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
        XrSourceStatus status=transfer(&owned,&limits,NULL);
        if(!point){CHECK(status==XR_SOURCE_OK);count=runtime_attempts;CHECK(count);}
        else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !owned.code);
        runtime_fail_at=SIZE_MAX;
        if(owned.code) {
            XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            XrXirHostExecutionRequest request={owned.code,&config,owned.entry,NULL,0};XrXirCallResult result={0};
            CHECK(xr_xir_host_execute(&request,&result)==XR_XIR_CALL_RETURNED);
            CHECK(result.value.type==XR_XIR_I64 && !result.value.payload);xr_xir_call_result_drop(&result);
        }
        transfer_close(&owned);
    }
    printf("actual SourceProduct/public Program/VM transfer sites=%zu; every OOM preserves both owners; producers dead before host execution; physical=0/0\n",count);
    return 0;
}
