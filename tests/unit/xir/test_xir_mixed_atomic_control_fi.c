/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_control_fi.c - Original runtime ordinals of complete control graphs
 */
#include "xir_mixed_atomic_control_resources.h"
static unsigned atomic_control_fi_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
static void atomic_control_fi_normal(const AtomicControlSpec *spec,AtomicResourceResult *normal) {
    *normal=atomic_budget_baseline(spec);CHECK(runtime_fail_at==SIZE_MAX && normal->complete && normal->sites && normal->sites<20000);
    printf("Atomic control FI census backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u sites=%zu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu injected0 twoIndependentNormalGraphs originalDomains physical0/0\n",
        spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,normal->sites,(unsigned long long)normal->cost[0].requested_bytes,(unsigned long long)normal->cost[0].requested_call_bytes,(unsigned long long)normal->cost[0].work,(unsigned long long)normal->cost[1].requested_bytes,(unsigned long long)normal->cost[1].requested_call_bytes,(unsigned long long)normal->cost[1].work);fflush(stdout);
}
static void atomic_control_fi_range(const AtomicControlSpec *spec,unsigned begin,unsigned end,const AtomicResourceResult *normal) {
    CHECK(begin<end && end<=normal->sites && normal->sites<20000);
    for(unsigned ordinal=begin;ordinal<end;++ordinal) {
        printf("Atomic control FI start backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u ordinal=%u ofSites%zu\n",spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,ordinal,normal->sites);fflush(stdout);
        AtomicBudgetResult observed=atomic_budget_transaction(spec,NULL,0,ordinal);AtomicResourceResult r=observed.resource;
        printf("Atomic control FI result backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u ordinal=%u fault=%u faultOwner=%u faultStep=%u faultEntry=%u faultObserved=%u publishedMask=%u selected=%u controlAttempted=%u controlCompleted=%u unaffected=%u/%u hit=%u physical0/0\n",
            spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,ordinal,r.fault,r.fault_owner,r.fault_step,r.fault_entry,r.fault_observed,r.published_mask,observed.selected,observed.control_attempted,observed.control_completed,(unsigned)r.unaffected[0],(unsigned)r.unaffected[1],(unsigned)r.hit);fflush(stdout);
        CHECK(!r.complete && r.hit && r.fault==XR_XIR_CALL_OOM && r.fault_owner<2 && r.unaffected[1-r.fault_owner]);
        unsigned healthy=1-r.fault_owner;CHECK(r.cost[healthy].requested_bytes==normal->cost[healthy].requested_bytes && r.cost[healthy].requested_call_bytes==normal->cost[healthy].requested_call_bytes && r.cost[healthy].work==normal->cost[healthy].work);
    }
    printf("Atomic control completeFI backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u range=%u:%u ofSites%zu everyOriginalOrdinal typedOOM physical0/0\n",
        spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,begin,end,normal->sites);fflush(stdout);
}
int main(int argc,char **argv) {
    CHECK(argc==10 || argc==14);(void)atomic_resource_original_main;(void)atomic_resource_axes;(void)atomic_budget_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    AtomicControlSpec spec={.backend=backend,.victim=atomic_control_fi_number(argv[3]),.call=atomic_control_fi_number(argv[4]),.entry=atomic_control_fi_number(argv[5])};
    if(!strcmp(argv[2],"census")) {
        CHECK(argc==10);spec.polls=atomic_control_fi_number(argv[6]);spec.mode=atomic_control_fi_number(argv[7]);spec.close=atomic_control_fi_number(argv[8]);spec.drop=atomic_control_fi_number(argv[9]);
        for(unsigned prefix=0;prefix<=spec.polls;++prefix){spec.prefix=prefix;AtomicResourceResult normal={0};atomic_control_fi_normal(&spec,&normal);}return 0;
    }
    CHECK(argc==14 && !strcmp(argv[2],"fi"));spec.prefix=atomic_control_fi_number(argv[6]);spec.polls=atomic_control_fi_number(argv[7]);spec.mode=atomic_control_fi_number(argv[8]);spec.close=atomic_control_fi_number(argv[9]);spec.drop=atomic_control_fi_number(argv[10]);
    unsigned begin=atomic_control_fi_number(argv[11]),end=atomic_control_fi_number(argv[12]),expected=atomic_control_fi_number(argv[13]);
    AtomicResourceResult normal={0};atomic_control_fi_normal(&spec,&normal);CHECK(normal.sites==expected);atomic_control_fi_range(&spec,begin,end,&normal);return 0;
}
