/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * emit_host_cli.c - Actual Source production and independent VM consumption
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "execution/xr_xir_host_cli.h"
#include "xir/xxir_output.h"
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(90); } } while (0)
static XrXirOutputStatus emit_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    (void)context;
    FILE *file=stream==XR_XIR_STDOUT ? stdout : stream==XR_XIR_STDERR ? stderr : NULL;
    CHECK(file);
    return fwrite(bytes,1,length,file)==length && !fflush(file) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
int main(int argc,char **argv) {
    CHECK(argc==5 && _setmode(_fileno(stdout),_O_BINARY)!=-1 && _setmode(_fileno(stderr),_O_BINARY)!=-1);
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,&context,argv[3],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source status=%u stage=%u detail=%s\n",(unsigned)status,
        (unsigned)diagnostic.stage,diagnostic.source.message);
    CHECK(status==XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    XrXirCSource generated={0};CHECK(xr_xir_compile_source_product_emit(product,"host_fixture",16777216,&generated)==XR_XIR_OK);
    FILE *output=fopen(argv[4],"wb");CHECK(output);
    CHECK(fwrite(generated.text,1,generated.length,output)==generated.length && !fclose(output));
    xr_xir_compile_c_source_free(&generated);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);xr_compile_resources_release(resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,emit_bytes,NULL,config.value_limit};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirHostExecutionRequest execution={program,&config,entry,NULL,0};XrXirCallResult result={0};
    XrXirCallStatus executed=xr_xir_host_execute(&execution,&result);
    xr_xir_compile_program_drop(program);
    int exit_code=4;
    if(executed==XR_XIR_CALL_RETURNED) {
        CHECK(result.value.type==XR_XIR_I64 && !result.value.reserved && !result.value.payload);exit_code=0;
    } else if(executed==XR_XIR_CALL_THROWN || xr_xir_call_panic_status(executed)) {
        CHECK(xr_xir_host_report_result(&result,(XrValueFormatSink){stderr,xr_value_format_file_write},false)==XR_XIR_HOST_REPORT_OK);
        CHECK(!fflush(stderr));exit_code=1;
    } else fprintf(stderr,"VM status=%u\n",(unsigned)executed);
    xr_xir_call_result_drop(&result);
    return exit_code;
}
