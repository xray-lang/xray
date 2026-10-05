/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_native.c - Source cleanup in generated native code without a VM
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
XR_DATA const XrXirProgramSpec cleanup_source_program;
XR_DATA const uint32_t cleanup_source_functions[CLEANUP_SOURCE_FUNCTIONS];
XR_DATA const uint32_t cleanup_source_fatal_functions[4];
static XrXirStatus cleanup_native_build(const XrXirCompileContext *context,unsigned variant,XrXirProgram **output) {
    (void)variant;XrXirStatus status=xr_xir_compile_program_seal(context,&cleanup_source_program,output);
    if(status!=XR_XIR_OK)CHECK(!*output);return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==3 && !strcmp(argv[1],"--fatal")));
    XrXirCompileContext context=cleanup_source_context((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline=cleanup_source_stats(&context);XrXirProgram *program=NULL;
    CHECK(cleanup_native_build(&context,0,&program)==XR_XIR_OK);
    if(argc==3) {
        unsigned mode=(unsigned)atoi(argv[2]);CHECK(mode<4);
        cleanup_source_fatal(program,cleanup_source_fatal_functions[mode]);
    }
    cleanup_source_primary_stats(&context,0);
    cleanup_source_allocations(program,cleanup_source_functions);cleanup_source_cases(program,cleanup_source_functions);program=NULL;
    cleanup_source_owner_free(&context,baseline);cleanup_source_compiler(cleanup_native_build,0);
    puts("Source cleanup native: independent output, pending results and physical release passed");return 0;
}
