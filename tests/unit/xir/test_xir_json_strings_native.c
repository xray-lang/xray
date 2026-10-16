/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_strings_native.c - Real C11 ordinary JSON token execution
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_strings_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_strings_program;
XR_DATA const uint32_t fixture_json_strings_selection[4];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_json_strings_program,&program)==XR_XIR_OK);
    json_strings_oracles(program,fixture_json_strings_selection);const char input[]="\"\\u6c49\\ud83d\\ude00\"";
    XrXirValue retained=json_strings_call(program,fixture_json_strings_selection[1],input,sizeof(input)-1);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    json_strings_expected(&retained,"汉😀",strlen("汉😀"));xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Ordinary JSON string token native C11/independent bytes/nullable after producer and Program death/physical0");return 0;
}
