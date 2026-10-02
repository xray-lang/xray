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
#include "../xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_format.c"
#include "execution/xr_xir_host_execution.c"
static unsigned stream_failure, stream_calls;
static int observed_setmode(int descriptor,int mode) {
    ++stream_calls;
    return stream_calls==stream_failure ? -1 : _setmode(descriptor,mode);
}
static size_t observed_write(const void *bytes,size_t size,size_t count,FILE *file) {
    return stream_failure==3 ? 0 : fwrite(bytes,size,count,file);
}
static int observed_flush(FILE *file) {return stream_failure==4 ? EOF : fflush(file);}
#define _setmode observed_setmode
#define fwrite observed_write
#define fflush observed_flush
#include "execution/xr_xir_host_cli.c"
#undef _setmode
#undef fwrite
#undef fflush
int main(int argc,char **argv) {
    CHECK((argc==5 || argc==6) && _setmode(_fileno(stdout),_O_BINARY)!=-1 && _setmode(_fileno(stderr),_O_BINARY)!=-1);
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
    if(argc==6){stream_failure=(unsigned)atoi(argv[5]);CHECK(stream_failure>=1 && stream_failure<=4);}
    size_t attempts=runtime_attempts;
    int exit_code=xr_xir_host_program_main(program,entry);
    CHECK(!runtime_live && !runtime_bytes);
    if(stream_failure==1 || stream_failure==2)CHECK(runtime_attempts==attempts);
    return exit_code;
}
