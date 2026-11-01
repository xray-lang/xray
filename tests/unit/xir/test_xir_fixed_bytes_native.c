/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_fixed_bytes_native.c - Real generated C over the independent byte oracle
 *
 * KEY CONCEPT:
 *   Sealing rechecks the embedded cold input before native entry execution.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fixed native %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"
#include "fixed_bytes_product.h"
XR_DATA const XrXirProgramSpec fixed_bytes_program;
XR_DATA const uint32_t fixed_bytes_entry;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&fixed_bytes_program,&program)==XR_XIR_OK);
    fixed_bytes_run(program,fixed_bytes_entry);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);return 0;
}
