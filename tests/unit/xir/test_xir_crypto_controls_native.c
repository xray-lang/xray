/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_crypto_controls_native.c - Generated C executes the authentic ref controls
 *
 * KEY CONCEPT:
 *   Native paths consume the same Source-generated Program and catalog IDs;
 *   expected byte contents and retired output are checked independently.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"crypto controls native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "crypto_controls_runtime.h"
XR_DATA const XrXirProgramSpec crypto_controls_program;
XR_DATA const uint32_t crypto_controls_selected[2];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&crypto_controls_program,&program)==XR_XIR_OK);
    crypto_controls_run(program,crypto_controls_selected[0],crypto_controls_selected[1]);
    xr_compile_resources_release(context.resources);puts("crypto generated-C refs, typed split and retirement PASS");return 0;
}
