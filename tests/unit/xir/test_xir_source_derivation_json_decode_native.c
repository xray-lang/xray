/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_json_decode_native.c - Native ordinary decoded init
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_json_decode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_decode_program;
XR_DATA const uint32_t fixture_json_decode_selection[5];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_json_decode_program,&program)==XR_XIR_OK);
    decode_oracles(program,fixture_json_decode_selection);XrXirValue value=decode_call(program,fixture_json_decode_selection[1],"42",2);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    decode_expected(&value,true,42);xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("JSON decoded initializer native C11/retained nullable after Program and ledger caller death/physical0");return 0;
}
