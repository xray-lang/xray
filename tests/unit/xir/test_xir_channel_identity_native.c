/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_identity_native.c - Execute actual generated Channel identity code
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_channel_identity_execution_cases.h"
extern const XrXirProgramSpec channel_identity_program;
extern const unsigned channel_identity_entry_ordinals[3];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-faults");CHECK(argc==1 || faults);
    XrCompileResourceLimits limits={67108864,8388608,128000000};XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&channel_identity_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    if (faults) channel_identity_runtime_faults(program,channel_identity_entry_ordinals[0]);
    else channel_identity_execute(program,channel_identity_entry_ordinals);
    puts("Channel actual generated native C: exact typed outputs, two instances and range failures PASS");return 0;
}
