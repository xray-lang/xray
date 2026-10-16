/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_worker.c - Actual worker arguments and retained terminal container results
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_worker_program.h"
#include "p09_hash_source_check.h"
#include "p09_hash_worker_input.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult result={0};CHECK(hash_source_check(&owner.context,worker_source,&result)==XR_XIR_OK);
    XrXirArtifact *lowered=hash_source_replay(&owner.context,&result);uint32_t entries[3],worker=0;
    worker_entries(xr_xir_compile_artifact_module(lowered),entries,&worker);
    if(argc==2) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"p09_hash_worker",1048576,&source)==XR_XIR_OK);
        CHECK(!strstr(source.text,"({"));FILE *file=fopen(argv[1],"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t p09_hash_worker_ids[3]={%u,%u,%u};\nconst uint32_t p09_hash_worker_callee=%u;\n",entries[0],entries[1],entries[2],worker)>0 && !fclose(file));
        xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
        worker_program_run(program,entries);
    }
    source_fixture_owner_free(&owner);puts("Source hash worker COW and escaped Task container ownership PASS");return 0;
}
