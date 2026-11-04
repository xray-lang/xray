/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dylib_native.c - Identical generated Dylib calls and owned result checks
 *
 * KEY CONCEPT:
 *   The native carrier seals the exact source-produced program and reuses the
 *   same DLL data, byte oracles and public Instance driver as the VM.
 */
#include "dylib_runtime.h"
extern const XrXirProgramSpec dylib_runtime_program;
extern const uint32_t dylib_runtime_ids[DYLIB_ENTRY_COUNT];
int main(void){
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before,after;
 CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);XrXirProgram *program=NULL;
 CHECK(xr_xir_compile_program_seal(&context,&dylib_runtime_program,&program)==XR_XIR_OK);dylib_execute(program,dylib_runtime_ids);xr_xir_compile_program_drop(program);
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes&&!runtime_live&&!runtime_bytes);
 xr_compile_resources_release(context.resources);puts("42");return 0;
}
