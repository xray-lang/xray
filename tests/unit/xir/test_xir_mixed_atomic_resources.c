/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_resources.c - Finite Atomic runtime and original owner boundaries
 */
#include "xir_mixed_atomic_resources.h"
static unsigned atomic_resource_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
static void atomic_resource_early_work(unsigned backend) {
    for(unsigned owner=0;owner<2;++owner) {
        XrXirInstanceConfig caps;CHECK(xr_xir_instance_config_init(&caps,sizeof(caps))==XR_XIR_CALL_READY);caps.work_limit=1;
        AtomicResourceResult r=atomic_resource_transaction(backend,0,0,&caps,owner,SIZE_MAX);
        CHECK(!r.complete && r.fault==XR_XIR_CALL_LIMIT && r.fault_owner==owner && r.unaffected[1-owner]);
        CHECK(r.cost[1-owner].requested_bytes==(owner?14512u:14424u) && r.cost[1-owner].requested_call_bytes==(owner?463423u:456455u) && r.cost[1-owner].work==(owner?1470u:1427u));
        if(r.published_mask&(1u<<owner))CHECK(r.cost[owner].work<=1);
        printf("Atomic early work backend=%u owner=%u cap=1 fault=%u faultStep=%u faultEntry=%u faultObserved=%u publishedMask=%u disposals=%u unaffected=%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu originalOwnerContinuation codeLease1 physical0/0\n",
            backend,owner,r.fault,r.fault_step,r.fault_entry,r.fault_observed,r.published_mask,r.disposals,(unsigned)r.unaffected[1-owner],(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);fflush(stdout);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==3 || argc==5 || argc==8);(void)atomic_resource_original_main;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"census")) {
        CHECK(argc==3);for(unsigned close=0;close<2;++close)for(unsigned drop=0;drop<2;++drop)(void)atomic_resource_baseline(backend,close,drop);return 0;
    }
    if(!strcmp(argv[2],"early-work")){CHECK(argc==3);atomic_resource_early_work(backend);return 0;}
    CHECK(argc>=5);unsigned close=atomic_resource_number(argv[3]),drop=atomic_resource_number(argv[4]);CHECK(close<2 && drop<2);
    AtomicResourceResult normal=atomic_resource_baseline(backend,close,drop);
    if(!strcmp(argv[2],"axes")){CHECK(argc==5);atomic_resource_axes(backend,close,drop,&normal);return 0;}
    CHECK(argc==8 && !strcmp(argv[2],"fi"));
    unsigned begin=atomic_resource_number(argv[5]),end=atomic_resource_number(argv[6]),expected=atomic_resource_number(argv[7]);
    CHECK(begin<end && end<=expected && normal.sites==expected);
    for(unsigned ordinal=begin;ordinal<end;++ordinal) {
        printf("Atomic runtime FI start backend=%u close=%u drop=%u ordinal=%u of%u\n",backend,close,drop,ordinal,expected);fflush(stdout);
        AtomicResourceResult r=atomic_resource_transaction(backend,close,drop,NULL,0,ordinal);
        printf("Atomic runtime FI result backend=%u close=%u drop=%u ordinal=%u fault=%u faultOwner=%u faultStep=%u faultEntry=%u faultObserved=%u publishedMask=%u unaffected=%u/%u hit=%u physical0/0\n",
            backend,close,drop,ordinal,r.fault,r.fault_owner,r.fault_step,r.fault_entry,r.fault_observed,r.published_mask,(unsigned)r.unaffected[0],(unsigned)r.unaffected[1],(unsigned)r.hit);fflush(stdout);
        CHECK(!r.complete && r.hit && r.fault==XR_XIR_CALL_OOM && r.fault_owner<2 && r.unaffected[1-r.fault_owner]);
    }
    printf("Atomic runtime completeFI backend=%u close=%u drop=%u range=%u:%u of%u everyOriginalOrdinal typedOOM physical0/0\n",backend,close,drop,begin,end,expected);return 0;
}
