/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_try_native.c - Real generated native message operations
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_channel_try_execution_cases.h"
#include "xir_channel_retry_execution_cases.h"
#include "xir_channel_rendezvous_backend_cases.h"
extern const XrXirProgramSpec channel_try_program;
extern const unsigned channel_try_entry_ordinal;
extern const unsigned channel_try_retry_ordinals[4];
extern const unsigned channel_try_rendezvous_ordinals[3];
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--runtime-faults")));
    XrCompileResourceLimits limits={67108864,8388608,128000000};XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&channel_try_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    if (argc==2) { rendezvous_backend_faults(program,channel_try_rendezvous_ordinals);channel_try_runtime_faults(program,channel_try_entry_ordinal); }
    else { channel_retry_execute(program,channel_try_retry_ordinals);rendezvous_backend_execute(program,channel_try_rendezvous_ordinals);channel_try_execute(program,channel_try_entry_ordinal); }
    puts(argc==2 ? "Channel try native runtime fault qualification PASS" : "Channel try real generated native C execution PASS");return 0;
}
