/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_json_native.c - Real generated JSON string owners
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_json_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_program;
XR_DATA const uint32_t fixture_json_selection[4];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&fixture_json_program,&program)==XR_XIR_OK);
    if(!faults) json_quote_oracles(program,fixture_json_selection[3]);
    if(!faults) json_runtime_entered_oom(program,fixture_json_selection[1]);
    size_t constant_sites=json_runtime_failures(program,fixture_json_selection[0],false,faults);
    size_t string_sites=json_runtime_failures(program,fixture_json_selection[1],true,faults);
    XrXirValue text=json_runtime_take(program,fixture_json_selection[1]);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    json_runtime_expected(&text,true);xr_xir_value_drop(&text);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary JSON native: real compiled C11, constant=%zu string=%zu sites, full FI=%u, result after Program/ledger caller death, physical0\n",
        constant_sites,string_sites,faults);return 0;
}
