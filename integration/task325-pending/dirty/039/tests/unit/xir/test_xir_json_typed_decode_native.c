/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_typed_decode_native.c - Real C11 typed initializer calls
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_typed_decode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_typed_program;
XR_DATA const uint32_t fixture_json_typed_selection[8];
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==4);JsonTypedKeys keys={"label","active","count"};if(argc==4)keys=(JsonTypedKeys){argv[1],argv[2],argv[3]};
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_json_typed_program,&program)==XR_XIR_OK);
    named_oracles(program,fixture_json_typed_selection,&keys);char text[256];
    int n=snprintf(text,sizeof(text),"{\"%s\":\"ok\",\"%s\":true,\"%s\":42}",keys.label,keys.active,keys.count);CHECK(n>0 && n<(int)sizeof(text));
    XrXirValue retained=named_call(program,fixture_json_typed_selection[1],text,(size_t)n);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    named_expected(&retained,(JsonTypedExpected){true,"ok",2,true,42});xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual name-selected JSON initializer C11/native/retained nullable/physical0");return 0;
}

