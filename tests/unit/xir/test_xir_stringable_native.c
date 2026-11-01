/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stringable_native.c - Native scalar interface witnesses
 *
 * KEY CONCEPT:
 *   The compiled source is emitted from the same cold checked declaration family.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Stringable native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "stringable_runtime.h"
XR_DATA const XrXirProgramSpec stringable_program;
XR_DATA const uint32_t stringable_entry;
int main(void) {
 XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&stringable_program,&program)==XR_XIR_OK);
 stringable_run(program,stringable_entry,"Box(42)\nBox(hello)\n(age, 25)\n");
 xr_compile_resources_release(context.resources);puts("Stringable actual generated C PASS");return 0;
}
