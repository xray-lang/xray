/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derived_squares_native.c - Actual generated native constant table receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_derived_squares_runtime.h"
XR_DATA const XrXirProgramSpec fixture_squares_program;
XR_DATA const uint32_t fixture_squares_selection[2];
int main(int argc,char **argv) {
    bool inject=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || inject);
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&derived_squares_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&fixture_squares_program,&program)==XR_XIR_OK);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    size_t sites=derived_squares_runtime_failures(program,fixture_squares_selection,reader,inject);
    xr_xir_compile_program_drop(program);xr_xir_domain_drop(reader);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary constant table native: checksum140 / 0,1,4,9,16,25,36,49/capacity8; runtime sites=%zu; full FI=%u; physical0\n",sites,inject);return 0;
}
