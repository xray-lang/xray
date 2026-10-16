/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_foreach_matrix.c - Bounded graphs preserve all ordered callback observations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
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
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);FILE *file=NULL;
    if(argc==2){file=fopen(argv[1],"wb");CHECK(file);}
    uint32_t ids[4]={0};
    for(uint32_t row=0;row<4;++row) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirSourceResult result={0};CHECK(hash_source_check(&owner.context,foreach_matrix_sources[row],&result)==XR_XIR_OK);
        XrXirArtifact *lowered=hash_source_replay(&owner.context,&result);
        ids[row]=foreach_matrix_entry(xr_xir_compile_artifact_module(lowered));
        if(file) {
            char prefix[64];CHECK(snprintf(prefix,sizeof(prefix),"p09_foreach_matrix_%u",row)>0);
            XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&source)==XR_XIR_OK);
            CHECK(!strstr(source.text,"({") && fwrite(source.text,1,source.length,file)==source.length);
            xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(lowered);
        } else {
            XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
            foreach_matrix_run(program,ids[row],foreach_matrix_yields[row]);
        }
        source_fixture_owner_free(&owner);
    }
    if(file) {
        CHECK(fprintf(file,"\nconst XrXirProgramSpec *const p09_foreach_matrix_programs[4]={&p09_foreach_matrix_0_program,&p09_foreach_matrix_1_program,&p09_foreach_matrix_2_program,&p09_foreach_matrix_3_program};\nconst uint32_t p09_foreach_matrix_ids[4]={%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3])>0);
        CHECK(!fclose(file));
    }
    puts("All four independent traversal graphs and original predicates PASS");return 0;
}
