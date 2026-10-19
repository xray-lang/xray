/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_cycle_batch_resources.c - Every measured Source prefix owns both original ledgers
 */
#include "xir_mixed_cycle_batch_resources.h"
static const unsigned cr_measured_polls[2][4][3]={
    {{34,45,12},{25,36,12},{25,36,12},{25,36,12}},
    {{39,53,15},{30,44,15},{30,44,15},{30,44,15}}
};
static unsigned cr_measured(const CycleResourceSpec *spec){const unsigned sizes[]={1,2,3,16};unsigned n=0;while(n<4 && sizes[n]!=spec->count)++n;CHECK(spec->shape<2 && n<4 && spec->kind<3);return cr_measured_polls[spec->shape][n][spec->kind];}
static void cr_suite(const CycleResourceSpec *base,unsigned last,bool axes) {
    CHECK(last==cr_measured(base));CycleResourceSpec spec=*base;for(unsigned prefix=0;prefix<=last;++prefix){spec.prefix=prefix;CycleResourceResult r=cr_baseline(&spec);if(axes)cr_axes(&spec,&r);}
}
static unsigned cr_number(const char *text) {char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;}
int main(int argc,char **argv) {
    CHECK(argc>=3);(void)cycle_resource_original_main;const char *names[]={"0","1","vm","native"};unsigned backend=0;while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"census")) {
        CHECK(argc==3);const unsigned sizes[]={1,2,3,16};for(unsigned shape=0;shape<2;++shape)for(unsigned n=0;n<4;++n)for(unsigned kind=0;kind<3;++kind){CycleResourceSpec spec={backend,shape,sizes[n],kind,0,UINT32_MAX,0,0};CycleResourceResult r=cr_baseline(&spec);CHECK(r.polls==cr_measured(&spec));printf("Cycle census backend=%u shape=%u ring=%u kind=%u polls=%u sites=%zu\n",backend,shape,sizes[n],kind,r.polls,r.sites);}return 0;
    }
    CHECK(argc>=10);CycleResourceSpec spec={backend,cr_number(argv[3]),cr_number(argv[4]),cr_number(argv[5]),cr_number(argv[6]),cr_number(argv[7]),cr_number(argv[8]),cr_number(argv[9])};
    if(!strcmp(argv[2],"diagnose")){CHECK(argc==10);CycleResourceResult normal=cr_baseline(&spec);XrXirInstanceConfig cap;CHECK(xr_xir_instance_config_init(&cap,sizeof(cap))==XR_XIR_CALL_READY);cap.work_limit=normal.cost[1].work;cr_diagnostic=true;CycleResourceResult limited=cr_operation(&spec,SIZE_MAX,&cap,1);printf("Cycle diagnostic exactWork=%llu complete=%u measured=%llu phase=%u\n",(unsigned long long)cap.work_limit,(unsigned)limited.complete,(unsigned long long)limited.cost[1].work,limited.phase);return 0;}
    if(!strcmp(argv[2],"suite") || !strcmp(argv[2],"suite-axes")){CHECK(argc==10);cr_suite(&spec,spec.prefix,!strcmp(argv[2],"suite-axes"));return 0;}
    if(!strcmp(argv[2],"baseline") || !strcmp(argv[2],"axes")){CHECK(argc==10);CycleResourceResult r=cr_baseline(&spec);if(!strcmp(argv[2],"axes"))cr_axes(&spec,&r);return 0;}
    CHECK(argc==13 && !strcmp(argv[2],"fi"));unsigned begin=cr_number(argv[10]),end=cr_number(argv[11]),expected=cr_number(argv[12]);CycleResourceResult normal=cr_baseline(&spec);CHECK(normal.sites==expected && begin<end && end<=expected);
    for(unsigned site=begin;site<end;++site){printf("Cycle FI start backend=%u shape=%u ring=%u kind=%u mode=%u prefix=%u close=%u drop=%u site=%u of%u\n",spec.backend,spec.shape,spec.count,spec.kind,spec.mode,spec.prefix,spec.close,spec.drop,site,expected);fflush(stdout);CycleResourceResult r=cr_operation(&spec,site,NULL,0);CHECK(!r.complete && r.hit);}
    printf("Cycle completeFI backend=%u shape=%u ring=%u kind=%u mode=%u prefix=%u close=%u drop=%u range=%u:%u of%u typedOOM originalDomains2 physical0/0\n",spec.backend,spec.shape,spec.count,spec.kind,spec.mode,spec.prefix,spec.close,spec.drop,begin,end,expected);return 0;
}
