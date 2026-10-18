/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_compile_cases.h - Complete compiler ordinals and three finite axes
 */
#ifndef XIR_CLASS_ACCESS_COMPILE_CASES_H
#define XIR_CLASS_ACCESS_COMPILE_CASES_H
static uint64_t access_compile_runs;
static void access_compile_census(const AccessCompileTrial *trial) {
    const AccessCompileCost *cost=&trial->single;
    printf("CLASS_ACCESS_COMPILER_CENSUS fixture=%s reject=%u sites=%zu status=%u allocated=%llu live=%llu peak=%llu work=%llu samePipeline=1 physical=0/0\n",
        trial->fixture->name,(unsigned)trial->fixture->rejected,cost->sites,(unsigned)cost->status,
        (unsigned long long)cost->stats.allocated_bytes,(unsigned long long)cost->stats.live_bytes,
        (unsigned long long)cost->stats.peak_bytes,(unsigned long long)cost->stats.work);
}
static void access_compile_tree(const AccessCompileTrial *trial,size_t depth,size_t expected_bytes) {
    CHECK(depth<32768);access_compile_depth=depth;
    AccessCompileCost cost=access_compile_operation(trial,NULL,false);CHECK(cost.hits==depth);
    if (depth) {
        CHECK(access_compile_site_bytes[access_compile_faults[depth-1]]==expected_bytes);
        CHECK(cost.status==XR_XIR_OUT_OF_MEMORY && !cost.complete);++access_compile_runs;
    } else CHECK(cost.complete);
    size_t *sites=cell_runtime_observer_new(cost.sites*sizeof(*sites));
    memcpy(sites,access_compile_site_bytes,cost.sites*sizeof(*sites));
    printf("CLASS_ACCESS_COMPILER_BRANCH fixture=%s depth=%zu sites=%zu hits=%zu status=%u physical=0/0\n",
        trial->fixture->name,depth,cost.sites,cost.hits,(unsigned)cost.status);
    size_t begin=depth?access_compile_faults[depth-1]+1:0;
    for (size_t ordinal=begin;ordinal<cost.sites;++ordinal) {
        CHECK(depth+1<32768);access_compile_faults[depth]=ordinal;
        access_compile_tree(trial,depth+1,sites[ordinal]);
    }
    cell_runtime_observer_free(sites);
}
static void access_compile_retry_all(const AccessCompileTrial *trial,const size_t *sites) {
    for (size_t ordinal=0;ordinal<trial->single.sites;++ordinal) {
        access_compile_depth=1;access_compile_faults[0]=ordinal;
        AccessCompileCost cost=access_compile_operation(trial,NULL,true);
        CHECK(cost.hits==1 && cost.first==XR_XIR_OUT_OF_MEMORY);
        CHECK(access_compile_site_bytes[ordinal]==sites[ordinal]);
        CHECK(cost.same_owner || (cost.bootstrap && !ordinal));
        if (cost.same_owner && !cost.fits) CHECK(cost.status==XR_XIR_BUDGET && !cost.complete);
        else CHECK(cost.complete);
        printf("CLASS_ACCESS_COMPILER_RETRY fixture=%s ordinal=%zu sites=%zu first=%u actual=%u sameOwner=%u bootstrap=%u fits=%u noRefund=1 physical=0/0\n",
            trial->fixture->name,ordinal,cost.sites,(unsigned)cost.first,(unsigned)cost.status,
            (unsigned)cost.same_owner,(unsigned)cost.bootstrap,(unsigned)cost.fits);
    }
}
static uint64_t access_compile_live_boundary(const AccessCompileTrial *trial) {
    uint64_t peak=trial->single.stats.peak_bytes;CHECK(peak>sizeof(XrCompileResources));
    XrCompileResourceLimits limits=access_compile_limits();limits.live_bytes=peak;access_compile_depth=0;
    AccessCompileCost cost=access_compile_operation(trial,&limits,false);CHECK(cost.complete && !cost.hits);
    uint64_t exact=cost.stats.peak_bytes;CHECK(exact<=peak);
    for (uint64_t remaining=peak;remaining;--remaining) {
        CHECK(exact>sizeof(XrCompileResources));limits.live_bytes=exact-1;
        cost=access_compile_operation(trial,&limits,false);CHECK(!cost.hits && cost.stats.peak_bytes<=limits.live_bytes);
        CHECK(cost.stats.allocated_bytes<limits.allocated_bytes && cost.stats.work<limits.work);
        if (cost.status==XR_XIR_BUDGET) return exact;
        CHECK(cost.complete && cost.stats.peak_bytes<exact);exact=cost.stats.peak_bytes;
    }
    CHECK(false);return 0;
}
static void access_compile_axes(const AccessCompileTrial *trial) {
    for (unsigned axis=0;axis<3;++axis) {
        uint64_t exact=axis==0?trial->single.stats.allocated_bytes:axis==1?
            access_compile_live_boundary(trial):trial->single.stats.work;CHECK(exact>1);
        for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=access_compile_limits();
            if (axis==0) limits.allocated_bytes=exact-minus;
            else if (axis==1) limits.live_bytes=exact-minus;
            else limits.work=exact-minus;
            access_compile_depth=0;AccessCompileCost cost=access_compile_operation(trial,&limits,false);
            printf("CLASS_ACCESS_COMPILER_AXIS fixture=%s axis=%u minus=%u exact=%llu actual=%u physical=0/0\n",
                trial->fixture->name,axis,minus,(unsigned long long)exact,(unsigned)cost.status);
            CHECK(!cost.hits);
            if (minus) CHECK(cost.status==XR_XIR_BUDGET && !cost.complete);
            else {CHECK(cost.complete);access_compile_endpoint(trial->fixture,cost.status);}
            CHECK(cost.stats.allocated_bytes<=limits.allocated_bytes && cost.stats.peak_bytes<=limits.live_bytes && cost.stats.work<=limits.work);
            if (axis!=0) CHECK(cost.stats.allocated_bytes<limits.allocated_bytes);
            if (axis!=1) CHECK(cost.stats.peak_bytes<limits.live_bytes);
            if (axis!=2) CHECK(cost.stats.work<limits.work);
        }
    }
}
static void access_compiler_cases(const char *operation) {
    for (unsigned i=0;i<ACCESS_SOURCE_CASE_COUNT;++i) {
        AccessCompileTrial trial={.fixture=&access_source_cases[i]};
        access_compile_depth=0;trial.single=access_compile_operation(&trial,NULL,false);
        CHECK(trial.single.complete && trial.single.sites && !trial.single.hits);access_compile_census(&trial);
        size_t *sites=cell_runtime_observer_new(trial.single.sites*sizeof(*sites));
        memcpy(sites,access_compile_site_bytes,trial.single.sites*sizeof(*sites));
        if (!strcmp(operation,"--compiler-fi")) {
            access_compile_runs=0;access_compile_tree(&trial,0,0);
            access_compile_retry_all(&trial,sites);
            printf("CLASS_ACCESS_COMPILER_CLOSURE fixture=%s baseline=%zu actualFailureRuns=%llu everySuffix=1 everySameOwnerRetry=1\n",
                trial.fixture->name,trial.single.sites,(unsigned long long)access_compile_runs);
        } else if (!strcmp(operation,"--compiler-axes")) access_compile_axes(&trial);
        else CHECK(!strcmp(operation,"--normal"));
        cell_runtime_observer_free(sites);access_compile_depth=0;access_physical_zero();
    }
}
#endif // XIR_CLASS_ACCESS_COMPILE_CASES_H
