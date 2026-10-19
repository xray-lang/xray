/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_named_decode_native.c - Real C11 name-selected initializer calls
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_named_decode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_named_program;
XR_DATA const uint32_t fixture_json_named_selection[5];
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);const char *left=argc==3?argv[1]:"left",*right=argc==3?argv[2]:"right";
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_json_named_program,&program)==XR_XIR_OK);
    named_oracles(program,fixture_json_named_selection,left,right);char text[256];
    int n=snprintf(text,sizeof(text),"{\"%s\":0,\"%s\":42}",left,right);CHECK(n>0 && n<(int)sizeof(text));
    XrXirValue retained=named_call(program,fixture_json_named_selection[1],text,(size_t)n);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    named_expected(&retained,true,42);xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual name-selected JSON initializer C11/native/retained nullable/physical0");return 0;
}
