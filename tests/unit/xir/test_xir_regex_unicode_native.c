/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_unicode_native.c - Actual ordinary Unicode generated native calls
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_regex_unicode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_unicode_program;
XR_DATA const uint32_t fixture_unicode_selection[5];
int main(void) {
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_unicode_program,&program)==XR_XIR_OK);
 unicode_range_oracles(program,fixture_unicode_selection[1]);unicode_name_oracles(program,fixture_unicode_selection[0]);XrXirInstance *instance=unicode_instance(program);XrXirValue value=unicode_call(instance,fixture_unicode_selection[2],NULL,0);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);unicode_folded_value(&value);xr_xir_value_drop(&value);
 CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);puts("Ordinary Unicode actual native C11,452 boundaries/84 aliases/unknowns/invalid scalars/folded8 after producer and Program death/physical0");return 0;
}
