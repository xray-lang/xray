/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_signal_native.c - The identical generated program and handler outcomes
 */
#include "root_signal_runtime.h"
extern const XrXirProgramSpec root_signal_program;
extern const uint32_t root_signal_ids[6];
int main(int argc,char **argv) {
    (void)sizeof(root_signal_text);
    bool console=argc==2 && !strcmp(argv[1],"--private-console"),detached=argc==2 && !strcmp(argv[1],"--detached");
    CHECK(console || detached || argc==1);
#if defined(XR_OS_WINDOWS)
    if (!console && !detached) {
        root_signal_isolate(L"--detached",DETACHED_PROCESS);root_signal_isolate(L"--private-console",CREATE_NEW_CONSOLE);
        puts("42");return 0;
    }
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,TRUE));
#endif
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;limits.profile_allocations=true;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&root_signal_program,&program)==XR_XIR_OK);
    if(detached)root_signal_detached(program,root_signal_ids);
    else {root_signal_cases(program,root_signal_ids);root_signal_owner_cases(program,root_signal_ids,&context);root_signal_stop_cases(program,root_signal_ids);root_signal_admission_cases(program,root_signal_ids);}
    xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes && xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
#if defined(XR_OS_WINDOWS)
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,FALSE));
#endif
    return 0;
}
