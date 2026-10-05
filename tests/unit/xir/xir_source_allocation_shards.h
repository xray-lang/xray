/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_SOURCE_ALLOCATION_SHARDS_H
#define XIR_SOURCE_ALLOCATION_SHARDS_H
enum { ALLOCATION_CASES=33, ALLOCATION_WORKERS=8 };
static const char *const allocation_case_names[ALLOCATION_CASES]={
    "coalloc","coalloc_early","call_storage","region_output","plan_context","conversion_budget",
    "generic_method_snapshot","implementation_snapshot","constraint_plain","constraint_mixed",
    "interface_snapshot","enum_snapshot_empty","enum_snapshot_payload","nominal_substitution",
    "nominal_snapshot_nodes","nominal_snapshot_declarations","constructed_snapshot","root_source",
    "array_source","method_source","requirement_receiver","requirement_generic","enum_source",
    "class_source","class_array_source","iteration_source","inference_unit","inference_unit_noncall",
    "inference_dead_catch","inference_interface","inference_ordinary","inference_unit_result","resolver_faults"};
static unsigned allocation_mode,allocation_index,allocation_case;
static size_t allocation_expected[ALLOCATION_CASES],allocation_seen_points[ALLOCATION_CASES];
static bool allocation_seen[ALLOCATION_CASES];
static FILE *allocation_report;
static bool allocation_pure(unsigned id){return id<6 || id==32;}
static unsigned allocation_number(const char *text,unsigned maximum){
    CHECK(text && *text);unsigned value=0;
    while(*text){CHECK(*text>='0' && *text<='9');unsigned digit=(unsigned)(*text++-'0');
        CHECK(digit<=maximum && value<=(maximum-digit)/10);value=value*10+digit;}
    return value;
}
static void allocation_dispatch(int argc,char **argv){
    if(argc==1)return;
    CHECK(argc>=2);
    if(!strcmp(argv[1],"--allocation-baseline")){CHECK(argc==3);allocation_mode=1;}
    else{
        CHECK(!strcmp(argv[1],"--allocation-shard") && argc==5);
        allocation_mode=2;allocation_index=allocation_number(argv[2],ALLOCATION_WORKERS-1);
        const char *text=argv[3];
        for(unsigned id=0;id<ALLOCATION_CASES;++id){
            char word[32];size_t n=0;
            while(*text && *text!=','){CHECK(n+1<sizeof(word));word[n++]=*text++;}
            word[n]=0;allocation_expected[id]=allocation_number(word,67108864);
            CHECK(allocation_pure(id)?allocation_expected[id]==0:allocation_expected[id]>0);
            if(id+1<ALLOCATION_CASES)CHECK(*text++==',');else CHECK(!*text);
        }
    }
    allocation_report=fopen(argv[argc-1],"wb");CHECK(allocation_report);
}
static size_t allocation_case_begin(unsigned id){
    CHECK(id<ALLOCATION_CASES && !allocation_seen[id]);allocation_seen[id]=true;allocation_case=id;
    return allocation_mode==2?allocation_expected[id]:0;
}
static size_t allocation_first(void){return allocation_mode==2?(size_t)allocation_index+1:0;}
static bool allocation_more(size_t site,size_t sites){return allocation_mode==1?site==0:site<=sites;}
static size_t allocation_next(size_t site){return site+(allocation_mode==2?ALLOCATION_WORKERS:1);}
static void allocation_record_physical(void){
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    CHECK(fprintf(allocation_report,"\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
        source_fixture_compile_live,source_fixture_compile_bytes)>0 && fflush(allocation_report)==0);
}
static void allocation_point(size_t ordinal,XrXirStatus status,bool empty){
    CHECK(status==XR_XIR_OUT_OF_MEMORY && empty && source_fixture_compile_injected);
    CHECK(source_fixture_compile_attempts>source_fixture_compile_fail_at);
    ++allocation_seen_points[allocation_case];
    if(!allocation_report)return;
    CHECK(fprintf(allocation_report,"{\"kind\":\"point\",\"case\":%u,\"ordinal\":%zu,\"attempts\":%zu,"
        "\"status\":%u,\"injected\":true,\"empty_output\":true,",allocation_case,ordinal,source_fixture_compile_attempts,status)>0);
    allocation_record_physical();
}
static void allocation_pure_point(const char *role,size_t ordinal){
    CHECK(allocation_pure(allocation_case) && source_fixture_compile_injected);
    CHECK(source_fixture_compile_attempts>source_fixture_compile_fail_at);
    if(!allocation_report)return;
    CHECK(fprintf(allocation_report,"{\"kind\":\"pure_point\",\"case\":%u,\"role\":\"%s\",\"ordinal\":%zu,"
        "\"status\":%u,\"attempts\":%zu,\"fail_at\":%zu,\"injected\":true,",allocation_case,role,ordinal,
        XR_XIR_OUT_OF_MEMORY,source_fixture_compile_attempts,source_fixture_compile_fail_at)>0);
    allocation_record_physical();
}
static void allocation_case_end(size_t sites){
    CHECK(allocation_pure(allocation_case)?sites==0:sites>0);
    if(allocation_mode==2){
        size_t expected=sites>allocation_index?(sites-allocation_index+ALLOCATION_WORKERS-1)/ALLOCATION_WORKERS:0;
        CHECK(sites==allocation_expected[allocation_case] && allocation_seen_points[allocation_case]==expected);
    }
    if(!allocation_report)return;
    CHECK(fprintf(allocation_report,"{\"kind\":\"case\",\"case\":%u,\"name\":\"%s\",\"sites\":%zu,\"points\":%zu,"
        "\"allocated_cap\":67108864,\"live_cap\":8388608,\"work_cap\":128000000,",allocation_case,
        allocation_case_names[allocation_case],sites,allocation_seen_points[allocation_case])>0);
    allocation_record_physical();
}
static void allocation_complete(void){
    for(unsigned id=0;id<ALLOCATION_CASES;++id)CHECK(allocation_seen[id]==(allocation_mode!=2 || !allocation_pure(id)));
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    if(allocation_report){
        CHECK(fprintf(allocation_report,"{\"kind\":\"complete\",\"mode\":%u,\"index\":%u,\"workers\":%u,",
            allocation_mode,allocation_index,ALLOCATION_WORKERS)>0);allocation_record_physical();
        CHECK(fclose(allocation_report)==0);allocation_report=NULL;
    }
}
#endif
