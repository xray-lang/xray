/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_hash_foreach_matrix_consumer.c - Admitted archives execute every callback graph
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xxir_compile_context.h"
#include "p09_hash_foreach_matrix_input.h"
#include "p09_hash_foreach_program.h"
extern const XrXirProgramSpec *const p09_foreach_matrix_programs[4];
extern const uint32_t p09_foreach_matrix_ids[4];
int main(void) {
    for(unsigned row=0;row<4;++row) {
        const XrCompileResourceLimits limits={16777216,16777216,64000000};
        XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_program_seal(&context,p09_foreach_matrix_programs[row],&program)==XR_XIR_OK);
        xr_compile_resources_release(context.resources);
        for(unsigned run=0;run<2;++run) {
            XrXirInstanceConfig config={0};XrXirInstance *instance=NULL;
            CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            XrXirValue result=foreach_call(instance,p09_foreach_matrix_ids[row],foreach_matrix_yields[row]);
            CHECK(result.type==XR_XIR_I64 && result.payload==123);xr_xir_value_drop(&result);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        }
        xr_xir_compile_program_drop(program);
    }
    puts("hash-foreach-matrix-sdk-ok");return 0;
}
