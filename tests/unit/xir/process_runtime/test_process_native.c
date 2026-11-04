/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_process_native.c - Identical generated Process calls and public wake handling
 *
 * KEY CONCEPT:
 *   The native carrier seals the exact source-produced program and reuses the
 *   same child protocol, byte oracles and public Instance driver as the VM.
 */
#include "process_runtime.h"
extern const XrXirProgramSpec process_runtime_program;
extern const uint32_t process_runtime_ids[PROCESS_ENTRY_COUNT];
int main(int argc,char **argv){
 if(process_child(argc,argv))return 0;CHECK(argc==1);
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before,after;
 CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);XrXirProgram *program=NULL;
 CHECK(xr_xir_compile_program_seal(&context,&process_runtime_program,&program)==XR_XIR_OK);process_execute(program,process_runtime_ids);xr_xir_compile_program_drop(program);
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes&&!runtime_live&&!runtime_bytes);
 xr_compile_resources_release(context.resources);puts("42");return 0;
}
