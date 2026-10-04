/* Test scheduling only: each ordinal uses the unchanged whole-graph builder. */
#ifndef XIR_SOURCE_NESTED_NULLABLE_METADATA_SHARDS_H
#define XIR_SOURCE_NESTED_NULLABLE_METADATA_SHARDS_H
static size_t sn_metadata_number(const char *text,size_t limit) {
    CHECK(text && *text);size_t value=0;
    for (;*text;++text) {
        CHECK(*text>='0' && *text<='9');size_t digit=(size_t)(*text-'0');
        CHECK(value<=limit/10 && (value<limit/10 || digit<=limit%10));value=value*10+digit;
    }
    return value;
}
static FILE *sn_metadata_report(const char *path) {
    FILE *report=fopen(path,"wb");CHECK(report);return report;
}
static size_t sn_metadata_shard_baseline(FILE *report,bool controls) {
    if (controls) { sn_source_rejections();sn_permission_control(); }
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    source_nested_compile_attempts=0;source_nested_compile_injected=false;
    source_nested_compile_fail_at=SIZE_MAX;sn_metadata_profile=true;
    XrXirProgram *program=NULL;uint32_t functions[SN_COUNT];
    XrXirStatus status=sn_build(&owner.context,NULL,&program,functions);
    size_t sites=source_nested_compile_attempts;sn_metadata_profile=false;
    CHECK(status==XR_XIR_OK && program && sites && sites<20000);
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    source_nested_compile_owner_free(&owner);
    CHECK(fprintf(report,"{\"kind\":\"baseline\",\"sites\":%zu,\"functions\":%u,\"oom_status\":%u,"
        "\"controls\":%s,\"allocated_cap\":67108864,\"live_cap\":8388608,\"work_cap\":128000000,"
        "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,\"runtime_blocks\":%zu,\"runtime_bytes\":%zu}\n",
        sites,(unsigned)SN_COUNT,(unsigned)XR_XIR_OUT_OF_MEMORY,controls?"true":"false",
        source_nested_compile_live,source_nested_compile_bytes,runtime_live,runtime_bytes)>0);
    CHECK(!fflush(report));return sites;
}
static void sn_metadata_shard_point(FILE *report,size_t ordinal) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    source_nested_compile_attempts=0;source_nested_compile_injected=false;
    source_nested_compile_fail_at=ordinal;sn_metadata_profile=false;
    XrXirProgram *program=NULL;uint32_t functions[SN_COUNT];
    XrXirStatus status=sn_build(&owner.context,NULL,&program,functions);
    size_t attempts=source_nested_compile_attempts;source_nested_compile_fail_at=SIZE_MAX;
    bool injected=source_nested_compile_injected;
    if (!(injected && attempts>ordinal && status==XR_XIR_OUT_OF_MEMORY && !program))
        fprintf(stderr,"metadata ordinal=%zu status=%u attempts=%zu injected=%u output=%u\n",
            ordinal,(unsigned)status,attempts,(unsigned)injected,(unsigned)(program!=NULL));
    CHECK(injected && attempts>ordinal && status==XR_XIR_OUT_OF_MEMORY && !program);
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    source_nested_compile_owner_free(&owner);
    CHECK(fprintf(report,"{\"kind\":\"point\",\"ordinal\":%zu,\"attempts\":%zu,\"status\":%u,"
        "\"injected\":true,\"empty_output\":true,\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,"
        "\"runtime_blocks\":%zu,\"runtime_bytes\":%zu}\n",ordinal,attempts,(unsigned)status,
        source_nested_compile_live,source_nested_compile_bytes,runtime_live,runtime_bytes)>0);
    CHECK(!fflush(report));
}
static bool sn_metadata_dispatch(int argc,char **argv) {
    if (argc<2) return false;
    if (!strcmp(argv[1],"--metadata-baseline")) {
        CHECK(argc==3);source_nested_compile_report=false;FILE *report=sn_metadata_report(argv[2]);
        (void)sn_metadata_shard_baseline(report,true);CHECK(!fclose(report));return true;
    }
    if (!strcmp(argv[1],"--metadata-shard")) {
        CHECK(argc==6);size_t index=sn_metadata_number(argv[2],15),workers=sn_metadata_number(argv[3],16);
        size_t expected=sn_metadata_number(argv[4],19999);CHECK(workers>=1 && workers<=16 && index<workers && expected);
        source_nested_compile_report=false;FILE *report=sn_metadata_report(argv[5]);
        size_t sites=sn_metadata_shard_baseline(report,false);
        if (sites!=expected) fprintf(stderr,"metadata shard %zu baseline=%zu coordinator=%zu\n",index,sites,expected);
        CHECK(sites==expected);
        for (size_t ordinal=index;ordinal<sites;ordinal+=workers) sn_metadata_shard_point(report,ordinal);
        CHECK(fprintf(report,"{\"kind\":\"complete\",\"index\":%zu,\"workers\":%zu,\"sites\":%zu}\n",index,workers,sites)>0);
        CHECK(!fclose(report));return true;
    }
    if (!strcmp(argv[1],"--metadata-bounds")) {
        CHECK(argc==3);source_nested_compile_report=false;FILE *report=sn_metadata_report(argv[2]);
        sn_metadata_bounds();
        CHECK(fprintf(report,"{\"kind\":\"bounds\",\"exact\":true,\"minus1\":[\"allocated\",\"live\",\"work\"],"
            "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,\"runtime_blocks\":%zu,\"runtime_bytes\":%zu}\n",
            source_nested_compile_live,source_nested_compile_bytes,runtime_live,runtime_bytes)>0);
        CHECK(!fclose(report));return true;
    }
    return false;
}
#endif
