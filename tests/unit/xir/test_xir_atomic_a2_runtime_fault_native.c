/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_atomic_a2_runtime_fault_cases.h"
extern const XrXirProgramSpec atomic_faults_program;
extern const unsigned atomic_fault_entries[2];
int main(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner.context,&atomic_faults_program,&program)==XR_XIR_OK);
    atomic_fault_cases(program,atomic_fault_entries);
    library_compile_owner_drop(&owner);library_compile_observer_free();return 0;
}
