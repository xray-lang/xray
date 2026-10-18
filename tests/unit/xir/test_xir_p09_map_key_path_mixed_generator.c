/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_mixed_generator.c - Emit real C11 callers for every container graph
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "p09_map_key_path_mixed_input.h"
int main(int argc,char **argv) {
    CHECK(argc==2);FILE *file=fopen(argv[1],"wb");CHECK(file);
    uint32_t entries[9][3]={{0}},targets[9][3]={{0}};
    for(uint32_t row=0;row<9;++row) {
        char text[8192];key_mixed_source(row,text,sizeof(text));
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirSourceResult result={0};
        CHECK(hash_source_check(&owner.context,text,&result)==XR_XIR_OK);
        XrXirArtifact *lowered=hash_source_replay(&owner.context,&result);
        key_mixed_entries(xr_xir_compile_artifact_module(lowered),row,entries[row],targets[row]);
        char prefix[64];CHECK(snprintf(prefix,sizeof(prefix),"p09_map_key_bridge_%u",row)>0);
        XrXirCSource generated={0};
        CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&generated)==XR_XIR_OK);
        CHECK(!strstr(generated.text,"({") && fwrite(generated.text,1,generated.length,file)==generated.length);
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
        source_fixture_owner_free(&owner);
    }
    CHECK(fprintf(file,"\nconst XrXirProgramSpec *const p09_key_bridge_programs[9]={")>0);
    for(unsigned row=0;row<9;++row)CHECK(fprintf(file,"%s&p09_map_key_bridge_%u_program",row?",":"",row)>0);
    CHECK(fprintf(file,"};\nconst uint32_t p09_key_bridge_entries[9][3]={")>0);
    for(unsigned row=0;row<9;++row)CHECK(fprintf(file,"%s{%u,%u,%u}",row?",":"",entries[row][0],entries[row][1],entries[row][2])>0);
    CHECK(fprintf(file,"};\nconst uint32_t p09_key_bridge_targets[9][3]={")>0);
    for(unsigned row=0;row<9;++row)CHECK(fprintf(file,"%s{%u,%u,%u}",row?",":"",targets[row][0],targets[row][1],targets[row][2])>0);
    CHECK(fprintf(file,"};\n")>0 && !fclose(file));return 0;
}
