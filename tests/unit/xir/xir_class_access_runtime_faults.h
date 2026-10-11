/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_faults.h - Full actual allocation suffixes and preparation
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_FAULTS_H
#define XIR_CLASS_ACCESS_RUNTIME_FAULTS_H
static uint64_t access_runtime_runs;
static bool access_same_event(const CellRuntimeAllocationEvent *a,const CellRuntimeAllocationEvent *b) {
    return a->ordinal==b->ordinal && a->bytes==b->bytes && a->kind==b->kind && a->line==b->line &&
        a->phase==b->phase && a->instance==b->instance && a->role==b->role && a->entry==b->entry &&
        !strcmp(a->file,b->file);
}
typedef struct AccessRuntimeTrial {
    XrXirProgram *program;
    const CellSourceEntries *entries;
    const AccessSourceCase *fixture;
    unsigned mode;
} AccessRuntimeTrial;
static AccessRuntimeCost access_trial(const AccessRuntimeTrial *trial,const XrXirInstanceConfig *config,unsigned pass) {
    return access_runtime_operation(trial->program,trial->entries,trial->fixture,trial->mode,config,pass);
}
static void access_runtime_tree(const AccessRuntimeTrial *trial,size_t depth,const CellRuntimeAllocationEvent *expected) {
    CHECK(depth<32768);cell_runtime_fault_count=depth;
    AccessRuntimeCost cost=access_trial(trial,NULL,0);CHECK(cost.hits==depth);
    if (depth) {
        CHECK(access_same_event(&cell_runtime_events[cell_runtime_faults[depth-1]],expected));
        if (access_runtime_retry && depth==1 && cost.recovered)
            CHECK(cost.complete && cost.failure==XR_XIR_CALL_READY);
        else CHECK(cost.failure==XR_XIR_CALL_OOM);
        ++access_runtime_runs;
    } else CHECK(cost.complete && cost.failure==XR_XIR_CALL_READY);
    CellRuntimeAllocationEvent *events=cell_runtime_observer_new(cost.sites*sizeof(*events));
    memcpy(events,cell_runtime_events,cost.sites*sizeof(*events));
    printf("CLASS_ACCESS_RUNTIME_BRANCH fixture=%s mode=%u depth=%zu sites=%zu hits=%zu failure=%u recovered=%u physical=0/0\n",
        trial->fixture->name,trial->mode,depth,cost.sites,cost.hits,(unsigned)cost.failure,(unsigned)cost.recovered);
    size_t begin=depth?cell_runtime_faults[depth-1]+1:0;
    for (size_t ordinal=begin;ordinal<cost.sites;++ordinal) {
        CHECK(depth+1<32768);cell_runtime_faults[depth]=ordinal;
        printf("CLASS_ACCESS_RUNTIME_SITE fixture=%s mode=%u depth=%zu ordinal=%zu bytes=%zu phase=%u owner=%u role=%u entry=%u file=%s line=%u\n",
            trial->fixture->name,trial->mode,depth+1,ordinal,events[ordinal].bytes,events[ordinal].phase,
            events[ordinal].instance,events[ordinal].role,events[ordinal].entry,events[ordinal].file,events[ordinal].line);
        access_runtime_tree(trial,depth+1,&events[ordinal]);
    }
    cell_runtime_observer_free(events);
}
static void access_runtime_prepare(const AccessRuntimeTrial *trial,const AccessRuntimeCost *baseline,
    const CellRuntimeAllocationEvent *events) {
    CHECK(baseline->prepare_end>baseline->prepare_begin);
    /* The first real Instance start is checked atomically. Poll sites include
     * every nested Class prepare, access, abort, cleanup and actual Task call. */
    for (size_t ordinal=0;ordinal<baseline->sites;++ordinal) {
        bool initial=ordinal>=baseline->prepare_begin && ordinal<baseline->prepare_end;
        if (!initial && events[ordinal].phase!=ACCESS_POLL && events[ordinal].phase!=ACCESS_CLOSE) continue;
        if (initial) CHECK(events[ordinal].phase==ACCESS_START && !events[ordinal].instance);
        cell_runtime_faults[0]=ordinal;access_runtime_retry=initial;
        access_runtime_tree(trial,1,&events[ordinal]);
    }
    access_runtime_retry=true;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.requested_call_limit=baseline->prepare_after.requested_call_bytes;
    CHECK(config.requested_call_limit && config.requested_call_limit<=UINT64_C(67108864));
    cell_runtime_fault_count=1;cell_runtime_faults[0]=baseline->prepare_end-1;
    AccessRuntimeCost cost=access_trial(trial,&config,0);
    CHECK(cost.hits==1 && cost.recovered && cost.failure==XR_XIR_CALL_LIMIT);
    CHECK(cost.prepare_failed.requested_call_bytes==config.requested_call_limit &&
        cost.prepare_after.requested_call_bytes==cost.prepare_failed.requested_call_bytes);
    printf("CLASS_ACCESS_RUNTIME_SAMEQUOTA fixture=%s mode=%u first=OOM retry=LIMIT sameOwner=1 noRefund=1 physical=0/0\n",
        trial->fixture->name,trial->mode);
    access_runtime_retry=false;
}
#endif // XIR_CLASS_ACCESS_RUNTIME_FAULTS_H
