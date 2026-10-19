/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_pull_resources.c - Cached and indexed real pull resource transactions
 */
#include "xir_mixed_iterator_pull_resources.h"
static unsigned iterator_pull_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return (unsigned)n;
}
int main(int argc,char **argv) {
    CHECK(argc==3 || argc==10);(void)iterator_graph;const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"baseline") || !strcmp(argv[2],"axes")) {
        CHECK(argc==3);
        for(unsigned pull=1;pull<=4;++pull) for(unsigned first=0;first<(pull==4?1u:2u);++first)
            for(unsigned mode=0;mode<5;++mode) for(unsigned victim=0;victim<(pull==4 || mode?2u:1u);++victim) {
                IteratorResourceSpec spec={backend,first,mode,victim,pull};IteratorResourceResult normal=iterator_resource_baseline(&spec);
                if(!strcmp(argv[2],"axes"))iterator_resource_axes(&spec,&normal);
            }
        return 0;
    }
    CHECK(argc==10 && !strcmp(argv[2],"fi"));
    IteratorResourceSpec spec={backend,iterator_pull_number(argv[4]),iterator_pull_number(argv[5]),iterator_pull_number(argv[6]),iterator_pull_number(argv[3])};
    unsigned begin=iterator_pull_number(argv[7]),end=iterator_pull_number(argv[8]),expected=iterator_pull_number(argv[9]);
    IteratorResourceResult normal=iterator_resource_baseline(&spec);CHECK(normal.sites==expected && begin<end && end<=expected);
    for(unsigned site=begin;site<end;++site) {
        printf("Iterator pull FI start backend=%u pull=%u first=%u mode=%u victim=%u site=%u of%u\n",spec.backend,spec.pull,spec.first,spec.mode,spec.victim,site,expected);fflush(stdout);
        IteratorResourceResult r=iterator_resource_operation(&spec,site,NULL);CHECK(!r.complete && r.hit);
    }
    printf("Iterator pull completeFI backend=%u pull=%u first=%u mode=%u victim=%u range=%u:%u of%u typedOOM originalDomain physical0/0\n",spec.backend,spec.pull,spec.first,spec.mode,spec.victim,begin,end,expected);return 0;
}
