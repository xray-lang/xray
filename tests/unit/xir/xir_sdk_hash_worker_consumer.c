/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_hash_worker_consumer.c - Actual admitted archives execute owned container programs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xxir_compile_context.h"
#include "p09_hash_worker_program.h"
extern const XrXirProgramSpec p09_hash_worker_program;
extern const uint32_t p09_hash_worker_ids[3];
int main(void) {
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&p09_hash_worker_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);worker_program_run(program,p09_hash_worker_ids);
    puts("hash-worker-sdk-ok");return 0;
}
