/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_precompile_native.c - Real generated regex program execution
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_regex_precompile_runtime.h"
XR_DATA const XrXirProgramSpec fixture_regex_program;
XR_DATA const uint32_t fixture_regex_selection[4];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&fixture_regex_program,&program)==XR_XIR_OK);XrXirValue values[3];
    for(unsigned which=0;which<3;++which) {
        size_t sites=regex_runtime_failures(program,fixture_regex_selection[which],which,faults);
        printf("ordinary regex native case%u: sites=%zu full FI=%u\n",which,sites,faults);
        values[which]=regex_runtime_take(program,fixture_regex_selection[which]);
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    for(unsigned which=0;which<3;++which) {regex_value(&values[which],which);xr_xir_value_drop(&values[which]);}
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("ordinary regex native real C11/independent case oracles/results after Program and ledger caller death/physical0");return 0;
}
