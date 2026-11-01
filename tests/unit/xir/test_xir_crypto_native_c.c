/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Crypto native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "crypto_native_runtime.h"
XR_DATA const XrXirProgramSpec crypto_demo_program,crypto_import_program;
XR_DATA const uint32_t crypto_demo_entry,crypto_import_entry;
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    const XrXirProgramSpec *specs[]={&crypto_demo_program,&crypto_import_program};
    const uint32_t entries[]={crypto_demo_entry,crypto_import_entry};const char *expected[]={"6\n","1\n"};
    for(unsigned i=0;i<2;++i){XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,specs[i],&program)==XR_XIR_OK);crypto_run(program,entries[i],expected[i]);}
    xr_compile_resources_release(context.resources);puts("Complete entropy demo and crypto import native PASS");return 0;
}
