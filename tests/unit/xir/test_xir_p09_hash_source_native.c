/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_source_native.c - Real generated Source hash carrier calls
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_program.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_program.h"
extern const XrXirProgramSpec p09_hash_source_program;
extern const uint32_t p09_hash_source_function_ids[4];
int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context, &p09_hash_source_program, &program) == XR_XIR_OK);
    hash_source_program_run(program, p09_hash_source_function_ids[1], p09_hash_source_function_ids[3]);
    source_fixture_owner_free(&owner);
    puts("Source Map/Set generated C11 calls, late results and COW PASS"); return 0;
}
