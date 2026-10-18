/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_callable_native.c - Generated C executes ordinary method factories
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_callable_cases.h"
#include "xir_callable_argument_admission.h"
XR_DATA const XrXirProgramSpec generator_callable_program;
XR_DATA const uint32_t generator_callable_ids[10];
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_callable_program,&program)==XR_XIR_OK);
    XrXirCallEntry entries[128];CHECK(generator_callable_program.entry_count<=128);
    memcpy(entries,generator_callable_program.entries,generator_callable_program.entry_count*sizeof(*entries));
    callable_admission_entries(entries,generator_callable_program.entry_count,generator_callable_program.types);
    entries[generator_callable_ids[9]].resume=callable_admission_resume;
    XrXirProgramSpec spec=generator_callable_program;spec.entries=entries;XrXirProgram *adapter=NULL,*foreign=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&adapter)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&foreign)==XR_XIR_OK);
    callable_admission_cases(adapter,foreign,generator_callable_ids,generator_callable_program.types);
    xr_xir_compile_program_drop(foreign);xr_xir_compile_program_drop(adapter);
    generator_callable_cases(program,generator_callable_ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Generator callable native physical0/0 PASS");return 0;
}
