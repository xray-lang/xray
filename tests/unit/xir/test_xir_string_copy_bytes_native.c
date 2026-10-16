/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_copy_bytes_native.c - Actual portable byte-copy native entries
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_string_copy_bytes_runtime.h"
XR_DATA const XrXirProgramSpec fixture_bytes_program;
XR_DATA const uint32_t fixture_bytes_selection[5];
int main(int argc,char **argv) {
 bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults);
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
 CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
 context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
 CHECK(xr_xir_compile_program_seal(&context,&fixture_bytes_program,&program)==XR_XIR_OK);XrXirValue values[4];
 for(unsigned which=0;which<4;++which){
  size_t sites=byte_runtime_failures(program,fixture_bytes_selection[which],which==2,faults);
  printf("UTF-8 bytes native case%u: sites=%zu full FI=%u\n",which,sites,faults);
  values[which]=byte_runtime_take(program,fixture_bytes_selection[which],which==2);
 }
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
 for(unsigned which=0;which<4;++which){byte_value(&values[which],which==2);xr_xir_value_drop(&values[which]);}
 CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
 puts("UTF-8 bytes native C11/independent byte oracle/isolated mutation/results after Program and ledger caller death/physical0");return 0;
}
