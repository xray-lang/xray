/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_path_prelude_native.c - The same retired Source products as real C entries
 *
 * KEY CONCEPT:
 *   Generated Program owners preserve the declaration-only initialization boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_internal.h"
#include "base/xresource_limits.h"
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Path native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_path_prelude_runtime.h"
#include "xir_path_complete_runtime.h"
XR_DATA const XrXirProgramSpec path_0_program,path_1_program,path_2_program;
XR_DATA const uint32_t path_0_main,path_1_main,path_2_main;
XR_DATA const XrXirProgramSpec path_complete_program;
XR_DATA const XrXirProgramSpec path_flavours_program;
XR_DATA const uint32_t path_flavours_ids[2];
XR_DATA const uint32_t path_complete_ids[PATH_COMPLETE_COUNT];
XR_DATA const XrXirProgramSpec path_complete_os_raw_program;
XR_DATA const uint32_t path_complete_os_raw_ids[PATH_COMPLETE_OS_COUNT];
int main(void) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    for(unsigned mode=0;mode<3;++mode) {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,mode==2?&path_2_program:mode?&path_1_program:&path_0_program,&program)==XR_XIR_OK);
        path_execute(program,mode==2?path_2_main:mode?path_1_main:path_0_main,mode==1?1:0);xr_xir_compile_program_drop(program);
    }
    XrXirProgram *complete=NULL;CHECK(xr_xir_compile_program_seal(&context,&path_complete_program,&complete)==XR_XIR_OK);
    XrXirProgram *raw=NULL;CHECK(xr_xir_compile_program_seal(&context,&path_complete_os_raw_program,&raw)==XR_XIR_OK);
    path_complete_execute(complete,path_complete_ids,raw,path_complete_os_raw_ids);xr_xir_compile_program_drop(raw);xr_xir_compile_program_drop(complete);
    XrXirProgram *flavours=NULL;CHECK(xr_xir_compile_program_seal(&context,&path_flavours_program,&flavours)==XR_XIR_OK);
    path_flavours_execute(flavours,path_flavours_ids);xr_xir_compile_program_drop(flavours);
    CHECK(!runtime_live&&!runtime_bytes);
    xr_compile_resources_release(context.resources);puts("Path declaration-only access and lexical shadow controls passed");return 0;
}
