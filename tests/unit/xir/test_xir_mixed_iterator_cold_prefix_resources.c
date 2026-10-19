/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_cold_prefix_resources.c - First pulls separate registered cleanup from cold frames
 */
#include "xir_mixed_iterator_cold_prefix_resources.h"
static unsigned iterator_cold_number(const char *text){char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;}
static const unsigned iterator_cold_polls[7]={23,23,23,39,51,4,4};
int main(int argc,char **argv) {
    CHECK(argc>=3);(void)iterator_prefix_normal_original_main;(void)iterator_prefix_budget_baseline;(void)iterator_prefix_budget_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"census")) {
        CHECK(argc==3);for(unsigned kind=0;kind<7;++kind)for(unsigned peer=0;peer<2;++peer)for(unsigned victim=0;victim<2;++victim){IteratorColdSpec spec={backend,kind,peer,victim,3,0};IteratorColdResult r=iterator_cold_baseline(&spec);unsigned registered=kind<5?1u:0u;CHECK(r.progress.polls==iterator_cold_polls[kind] && r.registered==registered && r.entered==registered && r.resource.cleanup==registered+peer);printf("Iterator cold census backend=%u kind=%u peer=%u victim=%u polls=%u registered=%u entered=%u cleanup=%u\n",backend,kind,peer,victim,r.progress.polls,r.registered,r.entered,r.resource.cleanup);}return 0;
    }
    if(!strcmp(argv[2],"baseline") || !strcmp(argv[2],"axes")) {
        CHECK(argc==6);unsigned kind=iterator_cold_number(argv[3]),peer=iterator_cold_number(argv[4]),mode=iterator_cold_number(argv[5]);CHECK(kind<7 && peer<2 && mode<3 && iterator_cold_polls[kind]);
        for(unsigned victim=0;victim<2;++victim)for(unsigned prefix=0;prefix<=iterator_cold_polls[kind];++prefix){IteratorColdSpec spec={backend,kind,peer,victim,mode,prefix};IteratorColdResult r=iterator_cold_baseline(&spec);if(!strcmp(argv[2],"axes"))iterator_cold_axes(&spec,&r);}return 0;
    }
    CHECK(argc==11 && !strcmp(argv[2],"fi"));IteratorColdSpec spec={backend,iterator_cold_number(argv[3]),iterator_cold_number(argv[4]),iterator_cold_number(argv[5]),iterator_cold_number(argv[6]),iterator_cold_number(argv[7])};
    unsigned begin=iterator_cold_number(argv[8]),end=iterator_cold_number(argv[9]),expected=iterator_cold_number(argv[10]);IteratorColdResult normal=iterator_cold_baseline(&spec);CHECK(normal.resource.sites==expected && begin<end && end<=expected);
    for(unsigned site=begin;site<end;++site){printf("Iterator cold FI start backend=%u kind=%u peer=%u victim=%u mode=%u prefix=%u site=%u of%u\n",spec.backend,spec.kind,spec.peer,spec.victim,spec.mode,spec.prefix,site,expected);fflush(stdout);IteratorColdResult r=iterator_cold_operation(&spec,site,NULL);CHECK(!r.resource.complete && r.resource.hit);}
    printf("Iterator cold completeFI backend=%u kind=%u peer=%u victim=%u mode=%u prefix=%u range=%u:%u of%u typedOOM originalDomain physical0/0\n",spec.backend,spec.kind,spec.peer,spec.victim,spec.mode,spec.prefix,begin,end,expected);return 0;
}
