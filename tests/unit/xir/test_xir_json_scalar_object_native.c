/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_scalar_native.c - Real C11 ordinary JSON token execution
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_scalar_object_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_scalar_object_program;
XR_DATA const uint32_t fixture_json_scalar_object_selection[8];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_json_scalar_object_program,&program)==XR_XIR_OK);
    json_scalar_oracles(program,fixture_json_scalar_object_selection);const char input[]="{\"label\":\"x\",\"active\":true,\"count\":42}";
    XrXirValue retained=json_scalar_call(program,fixture_json_scalar_object_selection[1],input,sizeof(input)-1);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    json_scalar_expected(&retained,(JsonScalarExpected){true,3,{"\"x\"","true","42"}});xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Ordinary JSON scalar object native C11/independent grammar/range/array after producer and Program death/physical0");return 0;
}
