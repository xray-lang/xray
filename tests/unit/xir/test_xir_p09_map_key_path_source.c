/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_source.c - Real source selectors and ordinary suspending ref calls
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "p09_map_key_path_input.h"
#include "xir/xxir_program.h"
#include "p09_map_key_path_program.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3); const size_t count=sizeof(map_key_sources)/sizeof(map_key_sources[0]);
    unsigned begin=0,end=(unsigned)count;
    FILE *file=NULL; uint32_t ids[8]={0};
    if(argc==3 && !strcmp(argv[1],"--case")) {begin=(unsigned)strtoul(argv[2],NULL,10);CHECK(begin<count);end=begin+1;}
    else if(argc==3) {CHECK(!strcmp(argv[1],"--emit"));file=fopen(argv[2],"wb");CHECK(file);}
    for(unsigned i=begin;i<end;++i) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner); XrXirSourceResult result={0};
        printf("key source case=%u name=%s\n",i,map_key_sources[i].name);fflush(stdout);
        CHECK(hash_source_check(&owner.context,map_key_sources[i].source,&result)==XR_XIR_OK);
        XrXirArtifact *lowered=hash_source_replay(&owner.context,&result);
        uint32_t entry=key_source_entry(xr_xir_compile_artifact_module(lowered));ids[i]=entry;
        if(file) {
            char prefix[64];CHECK(snprintf(prefix,sizeof(prefix),"p09_map_key_path_%u",i)>0);
            XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&generated)==XR_XIR_OK);
            CHECK(!strstr(generated.text,"({") && fwrite(generated.text,1,generated.length,file)==generated.length);
            xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
        } else {
            XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
            key_source_run(program,entry,map_key_sources[i].expected);
        }
        source_fixture_owner_free(&owner);
    }
    if(file) {
        CHECK(fprintf(file,"\nconst XrXirProgramSpec *const p09_map_key_path_programs[8]={")>0);
        for(unsigned i=0;i<8;++i)CHECK(fprintf(file,"%s&p09_map_key_path_%u_program",i?",":"",i)>0);
        CHECK(fprintf(file,"};\nconst uint32_t p09_map_key_path_ids[8]={")>0);
        for(unsigned i=0;i<8;++i)CHECK(fprintf(file,"%s%u",i?",":"",ids[i])>0);
        CHECK(fprintf(file,"};\n")>0 && !fclose(file));
    }
    puts("Source key COW, exact selector order, suspending ref, late roots and distinct bounds PASS");return 0;
}
