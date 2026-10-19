/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_host_call_native.c - Generated native host lifetime oracle
 */
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "execution/xr_xir_host_execution.c"
#include "xir_host_call_cases.h"
#include "xir_bounded_instance_cases.h"
XR_DATA const XrXirProgramSpec host_call_source_program;
XR_DATA const uint32_t host_call_entries[3];
XR_DATA const uint32_t host_call_fatal_entry;
XR_DATA const uint32_t host_call_bounded_entries[3];
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--fatal-drop")));
    XrXirProgram *program = NULL;
    XrCompileResources *resources=NULL;
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    CHECK(xr_xir_compile_program_seal(&context,&host_call_source_program,&program) == XR_XIR_OK);
    xr_compile_resources_release(resources);
    if (argc == 2) host_fatal_cleanup(program, host_call_fatal_entry);
    bounded_instances(program,host_call_entries[0],host_call_bounded_entries[0],host_call_bounded_entries[1],host_call_bounded_entries[2]);
    host_cases(program, host_call_entries);
    return 0;
}
