/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_fixed_array_scalar_native.c - Real generated C over the independent byte oracle
 *
 * KEY CONCEPT:
 *   Sealing rechecks the embedded cold input before native entry execution.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fixed native %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
#include "fixed_array_scalar_product.h"
XR_DATA const XrXirProgramSpec fixed_array_scalar_program;
XR_DATA const uint32_t fixed_array_scalar_entry;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_program_seal(&owner.context,&fixed_array_scalar_program,&program);
    if(status!=XR_XIR_OK)fprintf(stderr,"fixed native Program status%u\n",status);
    CHECK(status==XR_XIR_OK);
    fixed_array_scalar_run(program,fixed_array_scalar_entry);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);return 0;
}
