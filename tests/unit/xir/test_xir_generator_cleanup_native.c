/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_cleanup_native.c - Generated body exits retain exact cleanup
 *
 * KEY CONCEPT:
 *   Real generated and interpreted bodies share the canonical exit driver.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_cleanup_cases.h"
XR_DATA const XrXirProgramSpec generator_cleanup_program;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&owner.context,generator_cleanup_program.types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    generator_cleanup_cases(generator_cleanup_program.entries,generator_cleanup_program.entry_count,arena,domain);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Real generated-C Generator cleanup once across all exits; physical0 PASS");return 0;
}
