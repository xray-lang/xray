/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_native.c - Actual C11 key path programs and owned root lifetimes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_map_key_path_input.h"
#include "p09_map_key_path_program.h"
extern const XrXirProgramSpec *const p09_map_key_path_programs[8];
extern const uint32_t p09_map_key_path_ids[8];
int main(int argc,char **argv) {
    CHECK(argc==2);unsigned row=(unsigned)strtoul(argv[1],NULL,10);CHECK(row<8);
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,p09_map_key_path_programs[row],&program)==XR_XIR_OK);
    key_source_run(program,p09_map_key_path_ids[row],map_key_sources[row].expected);
    source_fixture_owner_free(&owner);printf("Actual C11 key path %s PASS\n",map_key_sources[row].name);return 0;
}
