/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_aggregate_encode_native.c - Actual aggregate encoding C11
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_aggregate_encode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_aggregate_program;
XR_DATA const uint32_t fixture_json_aggregate_selection[10];
int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
    XrXirCompileContext context=aggregate_runtime_context();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&fixture_json_aggregate_program,&program)==XR_XIR_OK);
    aggregate_runtime_faults(program,fixture_json_aggregate_selection,faults);XrXirValue values[6]={{0}};
    aggregate_runtime_results(program,fixture_json_aggregate_selection,values);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);aggregate_retained(values);
    puts("Actual aggregate encoding C11: six complete exact JSON outputs, comptime42 and final String lifetime/physical0");return 0;
}
