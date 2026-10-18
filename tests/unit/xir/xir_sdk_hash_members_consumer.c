/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_hash_members_consumer.c - Real generated hash instructions against an admitted SDK
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xxir_compile_context.h"
#include "p09_hash_members_program.h"
extern const XrXirProgramSpec p09_hash_members_program;
extern const uint32_t p09_hash_members_ids[5];
int main(void) {
    const XrCompileResourceLimits limits={16777216,16777216,64000000};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&p09_hash_members_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    members_program_run(program,p09_hash_members_ids);
    puts("hash-members-sdk-ok"); return 0;
}
