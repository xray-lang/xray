/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_a2_retry_native.c - Bounded backend retry and physical ownership tests
 *
 * KEY CONCEPT:
 *   Real production bodies retain captured operands across exactly one CAS per poll.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_atomic_a2_backend_cases.h"
extern const XrXirProgramSpec atomic_a2_program;
extern const unsigned atomic_a2_entry;
int main(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner.context,&atomic_a2_program,&program)==XR_XIR_OK);
    atomic_backend_cases(program,atomic_a2_entry);xr_xir_compile_program_drop(program);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("Native forced f64 retry: same poll frontier, one operand evaluation, consecutive RMW, cancellation and physical release passed");
    return 0;
}
