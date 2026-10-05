/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_native.c - Dedicated enum identity source and ownership execution
 *
 * KEY CONCEPT:
 *   The same owned Checked program supplies independent VM and native expectations.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_enum_identity_execution.h"
XR_DATA const XrXirProgramSpec fixture_enum_program;
XR_DATA const uint32_t fixture_enum_values[4],fixture_enum_count;
int main(void){
    SourceEnumIdentityEntries entries={{fixture_enum_values[0],fixture_enum_values[1],fixture_enum_values[2],fixture_enum_values[3]},fixture_enum_count};
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,&fixture_enum_program,&program)==XR_XIR_OK);
    XrXirValue retained[2][4]={{{0}}};
    source_enum_identity_pair(program,entries,retained);
    source_enum_identity_runtime_failures(program,entries);
    xr_xir_compile_program_drop(program);
    source_enum_identity_retained_drop(retained);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
    puts("enum native: independent 41, Box.Full, 703/401-byte strings");return 0;
}
