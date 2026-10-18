/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index_native.c - Actual generated C11 indexing and panic calls
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_map_index_input.h"
#include "xir_runtime_allocations.h"
#include "p09_map_index_program.h"
extern const XrXirProgramSpec p09_map_index_program;
extern const uint32_t p09_map_index_ids[INDEX_COUNT];
int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context, &p09_map_index_program, &program) == XR_XIR_OK);
    CHECK(index_source[0]);
    index_program_run(program, p09_map_index_ids); CHECK(!runtime_live && !runtime_bytes); source_fixture_owner_free(&owner);
    puts("Generated portable C11 Map index and E0431 PASS"); return 0;
}
