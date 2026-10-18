/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_foreach_matrix_native.c - Bounded graphs preserve all ordered callback observations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_foreach_matrix_input.h"
#include "p09_hash_foreach_program.h"
static void foreach_matrix_run(XrXirProgram *program,uint32_t entry,uint32_t expected) {
    for(unsigned run=0;run<2;++run) {
        XrXirInstanceConfig config={0};XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue result=foreach_call(instance,entry,expected);
        CHECK(result.type==XR_XIR_I64 && result.payload==123);xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
extern const XrXirProgramSpec *const p09_foreach_matrix_programs[4];
extern const uint32_t p09_foreach_matrix_ids[4];
int main(void) {
    for(uint32_t row=0;row<4;++row) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_program_seal(&owner.context,p09_foreach_matrix_programs[row],&program)==XR_XIR_OK);
        foreach_matrix_run(program,p09_foreach_matrix_ids[row],foreach_matrix_yields[row]);
        source_fixture_owner_free(&owner);
    }
    puts("All four traversal graphs in actual C11 PASS");return 0;
}
