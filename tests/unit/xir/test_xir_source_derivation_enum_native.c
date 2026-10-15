/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_enum_native.c - Real generated enum CFG execution
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_enum_runtime.h"
XR_DATA const XrXirProgramSpec fixture_enum_program;
XR_DATA const uint32_t fixture_enum_selection[5];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&fixture_enum_program,&program)==XR_XIR_OK);XrXirValue values[4];
    for(unsigned which=0;which<4;++which) {
        size_t sites=enum_runtime_failures(program,fixture_enum_selection[which],which,faults);
        printf("ordinary enum native case%u: sites=%zu full FI=%u\n",which,sites,faults);
        values[which]=enum_runtime_take(program,fixture_enum_selection[which]);
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    for(unsigned which=0;which<4;++which) {enum_runtime_expected(&values[which],which);xr_xir_value_drop(&values[which]);}
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("ordinary enum native real C11/independent case oracles/results after Program and ledger caller death/physical0");return 0;
}
