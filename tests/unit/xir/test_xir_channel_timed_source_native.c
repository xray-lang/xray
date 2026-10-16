/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_timed_native.c - Real generated blocking Channel body
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_channel_timed_execution_cases.h"
#include "xir_channel_timed_lifecycle_cases.h"
#include "xir_channel_timed_runtime_faults.h"
extern const XrXirProgramSpec channel_timed_program;
extern const unsigned channel_timed_entry_ordinal;
extern const unsigned channel_timed_endpoints[4];
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--host") || !strcmp(argv[1],"--runtime-faults"))));
    XrCompileResourceLimits limits={67108864,8388608,128000000};XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&channel_timed_program,&program)==XR_XIR_OK);xr_compile_resources_release(context.resources);
    if (argc==2 && !strcmp(argv[1],"--runtime-faults")) timed_source_runtime_faults(program,channel_timed_entry_ordinal);
    else if (argc==2) timed_source_host_execute(program,channel_timed_entry_ordinal);
    else { timed_life_cases(program,channel_timed_endpoints);timed_source_execute(program,channel_timed_entry_ordinal); }
    puts("Timed Channel real generated native C PASS");return 0;
}
