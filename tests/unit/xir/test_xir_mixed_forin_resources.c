/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_forin_resources.c - Cumulative limits and original allocation ordinals
 */
#include "xir_mixed_forin_resources.h"
static void forin_resource_early_work(unsigned backend) {
    for(unsigned scenario=0;scenario<12;++scenario)for(unsigned owner=0;owner<2;++owner) {
        ForinResourceSpec spec={backend,scenario,3,0,0,0};XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);config.work_limit=1;
        ForinResourceResult r=forin_resource_operation(&spec,SIZE_MAX,&config,owner);
        CHECK(!r.complete && r.fault==XR_XIR_CALL_LIMIT && r.fault_owner==owner && r.unaffected[1-owner]);
        if(r.published_mask&(1u<<owner))CHECK(r.cost[owner].work<=1);
        CHECK(r.cost[1-owner].requested_bytes && r.cost[1-owner].requested_call_bytes && r.cost[1-owner].work);
        printf("For-in early work backend=%u scenario=%u owner=%u cap=1 fault=%u faultPhase=%u faultObserved=%u publishedMask=%u published=%u disposals=%u unaffected=%u sites=%zu firstWork0=%llu firstWork1=%llu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu originalOwnerContinuation codeLease1 physical0/0\n",backend,scenario,owner,r.fault,r.fault_phase,r.fault_observed,r.published_mask,r.published,r.disposals,(unsigned)r.unaffected[1-owner],r.sites,(unsigned long long)r.fault_cost[0].work,(unsigned long long)r.fault_cost[1].work,(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);
    }
}
int main(int argc,char **argv) {
    CHECK(argc>=3);(void)forin_resource_original_main;const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"early-work")){CHECK(argc==3);forin_resource_early_work(backend);return 0;}
    if(!strcmp(argv[2],"census")) {
        CHECK(argc==3);
        for(unsigned scenario=0;scenario<12;++scenario)for(unsigned close=0;close<2;++close)for(unsigned drop=0;drop<2;++drop) {
            ForinResourceSpec spec={backend,scenario,3,0,close,drop};(void)forin_resource_baseline(&spec);
        }
        return 0;
    }
    CHECK(argc>=8);ForinResourceSpec spec={backend,forin_resource_number(argv[3]),forin_resource_number(argv[4]),forin_resource_number(argv[5]),forin_resource_number(argv[6]),forin_resource_number(argv[7])};
    CHECK(spec.scenario<12 && spec.mode<=3 && spec.close<2 && spec.drop<2);
    CHECK(spec.mode==3 || spec.prefix<=forin_resource_polls[spec.scenario]);
    if(!strcmp(argv[2],"suite")) {
        CHECK(argc==8 && spec.mode<3 && spec.prefix==forin_resource_polls[spec.scenario]);
        for(unsigned prefix=0;prefix<=spec.prefix;++prefix) {
            ForinResourceSpec current=spec;current.prefix=prefix;(void)forin_resource_baseline(&current);
        }
        return 0;
    }
    if(!strcmp(argv[2],"range") || !strcmp(argv[2],"range-axes")) {
        CHECK(argc==10 && spec.mode<3 && spec.prefix==forin_resource_polls[spec.scenario]);
        unsigned begin=forin_resource_number(argv[8]),end=forin_resource_number(argv[9]);CHECK(begin<end && end<=spec.prefix+1);
        for(unsigned prefix=begin;prefix<end;++prefix) {
            ForinResourceSpec current=spec;current.prefix=prefix;ForinResourceResult normal=forin_resource_baseline(&current);
            if(!strcmp(argv[2],"range-axes"))forin_resource_axes(&current,&normal);
        }
        return 0;
    }
    ForinResourceResult normal=forin_resource_baseline(&spec);
    if(!strcmp(argv[2],"baseline")){CHECK(argc==8);return 0;}
    if(!strcmp(argv[2],"axes")){CHECK(argc==8);forin_resource_axes(&spec,&normal);return 0;}
    CHECK(argc==11 && !strcmp(argv[2],"fi"));unsigned begin=forin_resource_number(argv[8]),end=forin_resource_number(argv[9]),expected=forin_resource_number(argv[10]);
    CHECK(begin<end && end<=expected && normal.sites==expected);
    for(unsigned site=begin;site<end;++site) {
        printf("For-in FI start backend=%u scenario=%u mode=%u prefix=%u close=%u drop=%u site=%u of%u\n",spec.backend,spec.scenario,spec.mode,spec.prefix,spec.close,spec.drop,site,expected);fflush(stdout);
        ForinResourceResult failed=forin_resource_operation(&spec,site,NULL,0);CHECK(!failed.complete && failed.hit && failed.fault==XR_XIR_CALL_OOM);
        CHECK(failed.fault_owner<2 && failed.unaffected[1-failed.fault_owner]);
    }
    printf("For-in completeFI backend=%u scenario=%u mode=%u prefix=%u close=%u drop=%u range=%u:%u of%u everyOriginalOrdinal typedOOM physical0/0\n",spec.backend,spec.scenario,spec.mode,spec.prefix,spec.close,spec.drop,begin,end,expected);
    return 0;
}
