/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_prefix_resources.c - Full original runtime resource boundaries for pull controls
 */
#include "xir_mixed_iterator_prefix_resources.h"
static unsigned iterator_prefix_budget_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
int main(int argc,char **argv) {
    CHECK(argc==5 || argc==10);(void)iterator_prefix_normal_original_main;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"baseline") || !strcmp(argv[2],"axes")) {
        CHECK(argc==5);unsigned kind=iterator_prefix_budget_number(argv[3]),mode=iterator_prefix_budget_number(argv[4]);CHECK(kind<4 && mode<3);
        for(unsigned victim=0;victim<2;++victim)for(unsigned prefix=0;prefix<=(kind==3?34u:22u);++prefix) {
            IteratorPrefixSpec spec={backend,kind,victim,mode,prefix};IteratorPrefixBudgetResult normal=iterator_prefix_budget_baseline(&spec);
            if(!strcmp(argv[2],"axes"))iterator_prefix_budget_axes(&spec,&normal);
        }
        return 0;
    }
    CHECK(argc==10 && !strcmp(argv[2],"fi"));
    IteratorPrefixSpec spec={backend,iterator_prefix_budget_number(argv[3]),iterator_prefix_budget_number(argv[4]),iterator_prefix_budget_number(argv[5]),iterator_prefix_budget_number(argv[6])};
    unsigned begin=iterator_prefix_budget_number(argv[7]),end=iterator_prefix_budget_number(argv[8]),expected=iterator_prefix_budget_number(argv[9]);
    IteratorPrefixBudgetResult normal=iterator_prefix_budget_baseline(&spec);CHECK(normal.resource.sites==expected && begin<end && end<=expected);
    for(unsigned site=begin;site<end;++site) {
        printf("Iterator prefix FI start backend=%u kind=%u victim=%u mode=%u prefix=%u site=%u of%u\n",spec.backend,spec.kind,spec.victim,spec.mode,spec.prefix,site,expected);fflush(stdout);
        IteratorPrefixBudgetResult result=iterator_prefix_budget_operation(&spec,site,NULL);CHECK(!result.resource.complete && result.resource.hit);
    }
    printf("Iterator prefix completeFI backend=%u kind=%u victim=%u mode=%u prefix=%u range=%u:%u of%u typedOOM originalDomain physical0/0\n",spec.backend,spec.kind,spec.victim,spec.mode,spec.prefix,begin,end,expected);return 0;
}
