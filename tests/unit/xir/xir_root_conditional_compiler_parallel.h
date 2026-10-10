/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_conditional_compiler_parallel.h - Complete compiler fault partitions
 *
 * KEY CONCEPT:
 *   Each trial replays all eight stages with one real finite owner. Processes
 *   partition physical ordinals, while the original serial reference remains.
 */
#ifndef XIR_ROOT_CONDITIONAL_COMPILER_PARALLEL_H
#define XIR_ROOT_CONDITIONAL_COMPILER_PARALLEL_H
enum { CONDITIONAL_PARALLEL_CASES=sizeof(root_conditional_oracles)/sizeof(root_conditional_oracles[0]) };
_Static_assert(CONDITIONAL_PARALLEL_CASES==12,"Complete conditional fixture inventory");
typedef struct ConditionalParallelRun {
    FILE *file;
    const char *mode;
    unsigned index,workers,fixture;
    size_t sites,points,axes;
    XrCompileResourceStats required;
} ConditionalParallelRun;
static void conditional_parallel_physical(FILE *file) {
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    CHECK(fprintf(file,"\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",
        source_program_compile_live,source_program_compile_bytes)>0);
    CHECK(fflush(file)==0);
}
static void conditional_parallel_baseline(ConditionalParallelRun *run) {
    RootConditionalCompilerIdentity identity={0};identity.digest=true;
    RootConditionalOperation operation={&root_conditional_oracles[run->fixture],false,false};
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    conditional_parallel_identity=&identity;source_program_compile_attempts=0;
    CHECK(conditional_operation(&owner.context,&operation)==XR_XIR_OK);
    conditional_parallel_identity=NULL;run->sites=source_program_compile_attempts;CHECK(run->sites);
    run->required=library_compile_stats(&owner.context);
    CHECK(run->required.live_bytes==owner.baseline.live_bytes && identity.phase==8);
    CHECK(identity.lengths[0] && identity.lengths[1]);
    char input_sha[65];conditional_parallel_hash(operation.oracle->text,strlen(operation.oracle->text),input_sha);
    library_compile_owner_drop(&owner);
    CHECK(fprintf(run->file,"{\"kind\":\"baseline\",\"case\":%u,\"name\":\"%s\",\"sites\":%zu,"
        "\"input_bytes\":%zu,\"input_sha256\":\"%s\",\"template_bytes\":%zu,\"template_sha256\":\"%s\","
        "\"instance_bytes\":%zu,\"instance_sha256\":\"%s\",\"allocation_count\":%llu,\"allocated\":%llu,"
        "\"live\":%llu,\"peak\":%llu,\"work\":%llu,\"allocated_cap\":67108864,\"live_cap\":8388608,"
        "\"work_cap\":128000000,\"oom_status\":%u,",run->fixture,operation.oracle->name,run->sites,
        strlen(operation.oracle->text),input_sha,identity.lengths[0],identity.sha[0],identity.lengths[1],identity.sha[1],
        (unsigned long long)run->required.allocation_count,(unsigned long long)run->required.allocated_bytes,
        (unsigned long long)run->required.live_bytes,(unsigned long long)run->required.peak_bytes,
        (unsigned long long)run->required.work,XR_XIR_OUT_OF_MEMORY)>0);
    conditional_parallel_physical(run->file);
}
static void conditional_parallel_faults(ConditionalParallelRun *run) {
    RootConditionalOperation operation={&root_conditional_oracles[run->fixture],false,false};
    for(size_t site=run->index;site<run->sites;site+=run->workers) {
        LibraryCompileOwner owner={0};RootConditionalCompilerIdentity identity={0};
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        conditional_parallel_identity=&identity;
        XrXirStatus status=conditional_operation(&owner.context,&operation);
        conditional_parallel_identity=NULL;source_program_compile_fail_at=SIZE_MAX;
        size_t attempts=source_program_compile_attempts;bool injected=source_program_compile_injected;
        if(status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"%s compiler fault %zu/%zu phase%u status%u\n",
            operation.oracle->name,site,run->sites,identity.phase,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && attempts>site);
        XrCompileResourceStats stats=library_compile_stats(&owner.context);
        CHECK(stats.live_bytes==owner.baseline.live_bytes);library_compile_owner_drop(&owner);
        CHECK(fprintf(run->file,"{\"kind\":\"point\",\"case\":%u,\"ordinal\":%zu,\"attempts\":%zu,"
            "\"phase\":%u,\"status\":%u,\"injected\":%s,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,",
            run->fixture,site,attempts,identity.phase,status,injected?"true":"false",
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work)>0);
        conditional_parallel_physical(run->file);++run->points;
    }
}
static void conditional_parallel_axes(ConditionalParallelRun *run) {
    RootConditionalOperation operation={&root_conditional_oracles[run->fixture],false,false};
    for(unsigned axis=0;axis<3;++axis) for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t exact=axis==0?run->required.allocated_bytes:axis==1?run->required.peak_bytes:run->required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(exact);*cap=exact-less;
        LibraryCompileOwner owner={0};RootConditionalCompilerIdentity identity={0};
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        conditional_parallel_identity=&identity;
        if(status==XR_XIR_OK) { source_program_compile_attempts=0;status=conditional_operation(&owner.context,&operation); }
        conditional_parallel_identity=NULL;
        if(status!=(less?XR_XIR_BUDGET:XR_XIR_OK)) fprintf(stderr,"%s axis%u less%u exact%llu status%u\n",
            operation.oracle->name,axis,less,(unsigned long long)exact,status);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        XrCompileResourceStats stats={0};if(owner.context.resources)stats=library_compile_stats(&owner.context);
        CHECK(stats.allocated_bytes<=limits.allocated_bytes && stats.peak_bytes<=limits.live_bytes && stats.work<=limits.work);
        library_compile_owner_drop(&owner);
        CHECK(fprintf(run->file,"{\"kind\":\"axis\",\"case\":%u,\"axis\":%u,\"shortfall\":%u,\"exact\":%llu,"
            "\"cap\":%llu,\"status\":%u,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,",run->fixture,axis,less,
            (unsigned long long)exact,(unsigned long long)*cap,status,(unsigned long long)stats.allocated_bytes,
            (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work)>0);
        conditional_parallel_physical(run->file);++run->axes;
    }
}
static unsigned conditional_parallel_argument(const char *text,unsigned maximum) {
    CHECK(text && *text);unsigned result=0;
    while(*text) {
        CHECK(*text>='0' && *text<='9');unsigned digit=(unsigned)(*text++-'0');
        CHECK(digit<=maximum && result<=(maximum-digit)/10);result=result*10+digit;
    }
    return result;
}
static int conditional_parallel_cli(int argc,char **argv) {
    if(argc<2 || (strcmp(argv[1],"--compiler-baseline") && strcmp(argv[1],"--compiler-shard") &&
        strcmp(argv[1],"--compiler-bounds"))) return 0;
    bool shard=!strcmp(argv[1],"--compiler-shard");CHECK(argc==(shard?5:3));
    ConditionalParallelRun run={0};run.mode=shard?"shard":!strcmp(argv[1],"--compiler-bounds")?"bounds":"baseline";
    run.index=shard?conditional_parallel_argument(argv[2],7):0;
    run.workers=shard?conditional_parallel_argument(argv[3],8):1;CHECK(run.workers && run.index<run.workers);
    run.file=fopen(argv[argc-1],"wb");CHECK(run.file);size_t total_sites=0,total_points=0,total_axes=0;
    for(run.fixture=0;run.fixture<CONDITIONAL_PARALLEL_CASES;++run.fixture) {
        run.points=0;run.axes=0;conditional_parallel_baseline(&run);
        if(shard)conditional_parallel_faults(&run);
        else if(!strcmp(run.mode,"bounds"))conditional_parallel_axes(&run);
        CHECK(fprintf(run.file,"{\"kind\":\"complete\",\"mode\":\"%s\",\"case\":%u,\"index\":%u,\"workers\":%u,"
            "\"sites\":%zu,\"points\":%zu,\"axes\":%zu,",run.mode,run.fixture,run.index,run.workers,run.sites,run.points,run.axes)>0);
        conditional_parallel_physical(run.file);total_sites+=run.sites;total_points+=run.points;total_axes+=run.axes;
    }
    CHECK(fprintf(run.file,"{\"kind\":\"finished\",\"mode\":\"%s\",\"cases\":%u,\"index\":%u,\"workers\":%u,"
        "\"sites\":%zu,\"points\":%zu,\"axes\":%zu,",run.mode,(unsigned)CONDITIONAL_PARALLEL_CASES,run.index,run.workers,
        total_sites,total_points,total_axes)>0);
    conditional_parallel_physical(run.file);CHECK(fclose(run.file)==0);return 1;
}
#endif // XIR_ROOT_CONDITIONAL_COMPILER_PARALLEL_H
