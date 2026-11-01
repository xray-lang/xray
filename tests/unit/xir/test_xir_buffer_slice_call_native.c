/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_slice_call_native.c - Same generated memory calls and leases
 *
 * KEY CONCEPT:
 *   Native code seals the exact cold input emitted by the Source consumer;
 *   its real callbacks must preserve both return scopes and metadata owners.
 */
#include "xir/xxir_vm.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"buffer native %d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "buffer_slice_call_cases.h"
XR_DATA const XrXirProgramSpec buffer_slice_call_program;
XR_DATA const uint32_t buffer_slice_call_ids[7];
int main(void) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&buffer_slice_call_program,&program)==XR_XIR_OK);
    buffer_call_cases(program,buffer_slice_call_ids);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);puts("Buffer native: genuine return scopes and metadata retirement PASS");return 0;
}
