/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_compiler_fault_shards.h - Exact whole-operation compiler fault partitions
 *
 * KEY CONCEPT:
 *   Every ordinal retains its original fresh finite owner and whole Source
 *   operation. A parent owns the single census, all partitions and six caps.
 */
#ifndef XIR_GENERIC_COMPILER_FAULT_SHARDS_H
#define XIR_GENERIC_COMPILER_FAULT_SHARDS_H
#include "base/xsha256.h"
#include <errno.h>

enum { GENERIC_COMPILER_FAULT_WORKERS = 8 };

static uint64_t generic_fault_number(const char *text) {
    CHECK(text && *text);
    for(const char *p=text;*p;++p)CHECK(*p>='0' && *p<='9');
    char *end=NULL;errno=0;unsigned long long value=strtoull(text,&end,10);
    CHECK(!errno && end && !*end);return (uint64_t)value;
}

static void generic_fault_hex(const char *text,size_t length) {
    CHECK(text && strlen(text)==length);
    for(size_t i=0;i<length;++i)CHECK((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f'));
}

static void generic_fault_preamble(FILE *report,const char *mode,uint64_t index,uint64_t sites,const char *nonce) {
    const char *input=getenv("XRAY_GENERIC_SHARD_INPUT_SHA256");
    const char *binary=getenv("XRAY_GENERIC_SHARD_BINARY_SHA256");
    const char *config=getenv("XRAY_GENERIC_SHARD_CONFIG_SHA256");
    generic_fault_hex(nonce,32);generic_fault_hex(input,64);generic_fault_hex(binary,64);generic_fault_hex(config,64);
    uint8_t digest[32];xr_sha256((const uint8_t *)source_generic_definition_text,sizeof(source_generic_definition_text)-1,digest);
    CHECK(fprintf(report,"{\"event\":\"identity\",\"schema\":\"generic-compiler-faults.v1\",\"mode\":\"%s\","
        "\"index\":%llu,\"sites\":%llu,\"workers\":%u,\"nonce\":\"%s\",\"input_sha256\":\"%s\","
        "\"binary_sha256\":\"%s\",\"config_sha256\":\"%s\",\"source_bytes\":%zu,\"source_sha256\":\"",
        mode,(unsigned long long)index,(unsigned long long)sites,GENERIC_COMPILER_FAULT_WORKERS,nonce,input,binary,config,
        sizeof(source_generic_definition_text)-1)>0);
    for(size_t i=0;i<sizeof(digest);++i)CHECK(fprintf(report,"%02x",(unsigned)digest[i])==2);
    CHECK(fprintf(report,"\",\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu}\n",
        (unsigned long long)library_compile_limits.allocated_bytes,(unsigned long long)library_compile_limits.live_bytes,
        (unsigned long long)library_compile_limits.work)>0);
}

static void generic_fault_stats(FILE *report,const XrCompileResourceStats *stats) {
    CHECK(fprintf(report,"\"allocated\":%llu,\"live\":%llu,\"peak\":%llu,\"work\":%llu,\"allocations\":%llu",
        (unsigned long long)stats->allocated_bytes,(unsigned long long)stats->live_bytes,
        (unsigned long long)stats->peak_bytes,(unsigned long long)stats->work,
        (unsigned long long)stats->allocation_count)>0);
}

static void generic_fault_complete(FILE *report,size_t points) {
    library_compile_observer_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes && source_program_compile_fail_at==SIZE_MAX);
    CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
        points,source_program_compile_live,source_program_compile_bytes)>0);
    CHECK(!ferror(report) && fflush(report)==0);CHECK(fclose(report)==0);
}

static void generic_fault_census(FILE *report) {
    LibraryCompileOwner owner={0};bool inspect=false;
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;sgm_enabled=true;sgm_calls=0;
    CHECK(source_generic_definition_operation(&owner.context,&inspect)==XR_XIR_OK);
    sgm_enabled=false;CHECK(sgm_calls==8);
    size_t sites=source_program_compile_attempts;CHECK(sites && sites<=UINT64_C(67108864));
    XrCompileResourceStats required=library_compile_stats(&owner.context);
    CHECK(required.live_bytes==owner.baseline.live_bytes);
    uint64_t baseline=owner.baseline.live_bytes;library_compile_owner_drop(&owner);
    CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"public_calls\":%u,\"owner_live\":%llu,",
        sites,sgm_calls,(unsigned long long)baseline)>0);generic_fault_stats(report,&required);
    CHECK(fprintf(report,",\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
        source_program_compile_live,source_program_compile_bytes)>0);generic_fault_complete(report,0);
}

static void generic_fault_partition(FILE *report,size_t index,size_t sites) {
    bool inspect=false;size_t points=0;
    for(size_t ordinal=index;ordinal<sites;ordinal+=GENERIC_COMPILER_FAULT_WORKERS) {
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=ordinal;source_program_compile_injected=false;
        XrXirStatus status=source_generic_definition_operation(&owner.context,&inspect);
        source_program_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        size_t attempts=source_program_compile_attempts;CHECK(attempts>ordinal);
        XrCompileResourceStats stats=library_compile_stats(&owner.context);uint64_t baseline=owner.baseline.live_bytes;
        CHECK(stats.live_bytes==baseline);library_compile_owner_drop(&owner);
        CHECK(fprintf(report,"{\"event\":\"point\",\"ordinal\":%zu,\"status\":%u,\"injected\":true,"
            "\"attempts\":%zu,\"owner_live\":%llu,",ordinal,(unsigned)status,attempts,(unsigned long long)baseline)>0);
        generic_fault_stats(report,&stats);
        CHECK(fprintf(report,",\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
            source_program_compile_live,source_program_compile_bytes)>0);++points;
    }
    generic_fault_complete(report,points);
}

static void generic_fault_bounds(FILE *report,const uint64_t required[3]) {
    bool inspect=false;
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(required[axis] && required[axis]<=*cap);*cap=required[axis]-less;
        LibraryCompileOwner owner={0};source_program_compile_attempts=0;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if(status==XR_XIR_OK)status=source_generic_definition_operation(&owner.context,&inspect);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        XrCompileResourceStats stats={0};uint64_t baseline=0;
        if(owner.context.resources){stats=library_compile_stats(&owner.context);baseline=owner.baseline.live_bytes;CHECK(stats.live_bytes==baseline);}
        library_compile_owner_drop(&owner);
        CHECK(fprintf(report,"{\"event\":\"axis\",\"axis\":%u,\"less\":%u,\"exact\":%llu,\"status\":%u,"
            "\"owner_live\":%llu,\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu,",
            axis,less,(unsigned long long)required[axis],(unsigned)status,(unsigned long long)baseline,
            (unsigned long long)limits.allocated_bytes,(unsigned long long)limits.live_bytes,(unsigned long long)limits.work)>0);
        generic_fault_stats(report,&stats);
        CHECK(fprintf(report,",\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
            source_program_compile_live,source_program_compile_bytes)>0);
    }
    generic_fault_complete(report,6);
}

static bool generic_fault_dispatch(int argc,char **argv) {
    if(argc<2)return false;
    bool census=!strcmp(argv[1],"--compiler-census"),shard=!strcmp(argv[1],"--compiler-shard");
    bool bounds=!strcmp(argv[1],"--compiler-bounds");if(!census && !shard && !bounds)return false;
    CHECK(argc==(census?4:shard?6:8));size_t index=0,sites=0;uint64_t required[3]={0};
    if(!census){uint64_t n=generic_fault_number(argv[shard?3:2]);CHECK(n && n<=UINT64_C(67108864));sites=(size_t)n;}
    if(shard){uint64_t n=generic_fault_number(argv[2]);CHECK(n<GENERIC_COMPILER_FAULT_WORKERS);index=(size_t)n;}
    if(bounds)for(unsigned axis=0;axis<3;++axis)required[axis]=generic_fault_number(argv[3+axis]);
    const char *nonce=argv[argc-2];generic_fault_hex(nonce,32);
    FILE *report=fopen(argv[argc-1],"wb");CHECK(report);
    generic_fault_preamble(report,census?"census":shard?"shard":"bounds",index,sites,nonce);
    if(census)generic_fault_census(report);
    else if(shard)generic_fault_partition(report,index,sites);
    else generic_fault_bounds(report,required);
    return true;
}
#endif // XIR_GENERIC_COMPILER_FAULT_SHARDS_H
