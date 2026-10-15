/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_native.c - Actual native ordinary derived method
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}} while(0)
#include "xir_source_derivation_runtime.h"
XR_DATA const XrXirProgramSpec fixture_derive_program;
XR_DATA const uint32_t fixture_derive_selection[2];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_derive_program,&program)==XR_XIR_OK);
    size_t sites=derive_runtime_failures(program,fixture_derive_selection[0],faults);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary derived method native42; runtime sites=%zu full FI=%u; physical0/0\n",sites,faults);return 0;
}
