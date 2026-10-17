/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effect_term_scratch.c - Sparse substitutions retain no earlier result
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir/xxir_effects.c"

typedef struct TermScratchFixture {
    XrXirTypeNode nodes[1036];
    XrXirCallableParameter fields[4][3];
    XrXirTypes types;
} TermScratchFixture;
static void term_fixture(TermScratchFixture *f,uint32_t mode) {
    memset(f,0,sizeof(*f));
    const XrXirType arguments[]={XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL};
    for (uint32_t n=0;n<4;++n) {
        uint32_t at=n*3;XrXirType array=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+at);
        f->nodes[at]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=arguments[n],.parameter_span=n?0:1};
        f->nodes[at+1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=arguments[n],.parameter_span=n?0:1};
        f->fields[n][0]=(XrXirCallableParameter){array,0};
        f->fields[n][1]=(XrXirCallableParameter){(XrXirType)(array+1),0};
        f->fields[n][2]=(XrXirCallableParameter){array,0};
        f->nodes[at+2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[n],.parameter_count=3,.parameter_span=n?0:1};
    }
    uint32_t count=mode==1?1036:mode>=2?3:12;
    for (uint32_t n=12;n<count;++n) f->nodes[n]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I32};
    if (mode==3) {
        f->fields[0][2].type=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);f->nodes[2].parameter_span=2;
    }
    f->types=(XrXirTypes){f->nodes,count,NULL,NULL};
}
static XrXirStatus term_operation(const XrXirCompileContext *context,uint32_t mode,bool oracle,uint64_t *repeated) {
    TermScratchFixture f;term_fixture(&f,mode);
    EffectTerms pool={.types=f.types,.capacity=f.types.count,.remaining=context};
    const XrXirType arguments[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL,XR_XIR_I64,XR_XIR_STRING,XR_XIR_I64};
    XrXirStatus status=XR_XIR_OK;uint64_t cost=0;
    for (uint32_t n=0;n<6 && status==XR_XIR_OK;++n) {
        XrXirGeneric generic={.arguments=&arguments[n],.argument_count=1};XrXirType result=XR_XIR_UNIT;
        uint64_t before=rp_stats(context).work;
        status=effect_terms_substitute(&pool,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),&generic,&result);
        uint64_t after=rp_stats(context).work;CHECK(!pool.stack_claimed && after>=before);
        if (n==5) cost=after-before;
        if (status==XR_XIR_OK && oracle) {
            uint32_t selected=arguments[n]==XR_XIR_I64?1:arguments[n]==XR_XIR_STRING?2:3;
            CHECK(result==(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+selected*3+2));
            CHECK(xr_xir_compile_type_substitution_matches_between(context,&f.types,&pool.types,
                &arguments[n],1,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),result)==XR_XIR_OK);
        }
    }
    effect_terms_free(&pool);if (status==XR_XIR_OK && repeated) *repeated=cost;return status;
}
static uint64_t term_semantics(uint32_t mode) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes,cost=0;
    CHECK(term_operation(&context,mode,true,&cost)==(mode==3?XR_XIR_BAD_TYPE:XR_XIR_OK));
    rp_owner_free(&context,baseline);rp_balanced(physical);return cost;
}
static void term_resources(uint32_t mode,bool all) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;XrXirStatus expected=mode==3?XR_XIR_BAD_TYPE:XR_XIR_OK;
    rp_attempts=0;CHECK(term_operation(&context,mode,false,NULL)==expected);
    size_t sites=rp_attempts;XrCompileResourceStats required=rp_stats(&context);CHECK(sites);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t i=0;i<sites;++i) {
        if (!all && i && i+1!=sites) continue;
        context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;RootParameterMark retained=rp_mark();
        rp_attempts=0;rp_fail_at=i;rp_injected=false;
        XrXirStatus status=term_operation(&context,mode,false,NULL);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);rp_balanced(retained);
        XrCompileResourceStats paid=rp_stats(&context);CHECK(paid.live_bytes==baseline);
        CHECK(term_operation(&context,mode,true,NULL)==expected);
        CHECK(rp_stats(&context).allocated_bytes>paid.allocated_bytes && rp_stats(&context).work>paid.work);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits caps=rp_caps();
        if (!axis) caps.allocated_bytes=required.allocated_bytes-less;
        else if (axis==1) caps.live_bytes=required.peak_bytes-less;
        else caps.work=required.work-less;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        XrXirStatus status=term_operation(&context,mode,false,NULL);
        if (status!=(less?XR_XIR_BUDGET:expected)) fprintf(stderr,"term mode%u axis%u less%u status%u expected%u\n",mode,axis,less,status,less?XR_XIR_BUDGET:expected);
        CHECK(status==(less?XR_XIR_BUDGET:expected));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    printf("Term scratch mode%u sites=%zu injection=%s exact/minus-one allocated/live/work same-owner retry physical0/0 PASS\n",mode,sites,all?"ALL":"FIRST_LAST_ONLY");
}
typedef struct TermDirectCost { uint64_t work,allocated; } TermDirectCost;
static TermDirectCost term_direct_parameter(uint32_t mode) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    TermScratchFixture f;term_fixture(&f,mode);if (mode==2) f.types.count=0;
    EffectTerms pool={.types=f.types,.capacity=f.types.count,.remaining=&context};
    const XrXirType arguments[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL};
    TermDirectCost cost={0};
    for (uint32_t n=0;n<6;++n) {
        XrXirGeneric generic={.arguments=&arguments[n%3],.argument_count=1};XrXirType result=XR_XIR_UNIT;
        XrCompileResourceStats before=rp_stats(&context);
        CHECK(effect_terms_substitute(&pool,XR_XIR_TYPE_PARAMETER_BASE,&generic,&result)==XR_XIR_OK);
        XrCompileResourceStats after=rp_stats(&context);
        cost.work+=after.work-before.work;cost.allocated+=after.allocated_bytes-before.allocated_bytes;
        CHECK(result==arguments[n%3] && !pool.stack_claimed && !pool.cache_count);
        CHECK(xr_xir_compile_type_substitution_matches_between(&context,&f.types,&pool.types,
            generic.arguments,1,XR_XIR_TYPE_PARAMETER_BASE,result)==XR_XIR_OK);
    }
    XrXirGeneric generic={.arguments=arguments,.argument_count=1};XrXirType rejected=XR_XIR_I64;
    CHECK(effect_terms_substitute(&pool,(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),&generic,&rejected)==XR_XIR_BAD_TYPE &&
        rejected==XR_XIR_UNIT && !pool.stack_claimed && !pool.cache_count);
    effect_terms_free(&pool);rp_owner_free(&context,baseline);rp_balanced(physical);return cost;
}

int main(int argc,char **argv) {
    bool all=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || all);
    TermDirectCost direct[3];
    for (uint32_t mode=0;mode<3;++mode) {
        direct[mode]=term_direct_parameter(mode);
        printf("Direct parameter mode%u six substitutions work=%llu allocated=%llu\n",mode,
            (unsigned long long)direct[mode].work,(unsigned long long)direct[mode].allocated);
    }
    CHECK(direct[0].work==direct[1].work && direct[1].work==direct[2].work && direct[0].work<32);
    CHECK(!direct[0].allocated && !direct[1].allocated && !direct[2].allocated);
    uint64_t narrow=term_semantics(0),wide=term_semantics(1);
    printf("Repeated sparse substitution work narrow=%llu wide=%llu unrelated_types=1024\n",(unsigned long long)narrow,(unsigned long long)wide);
    CHECK(narrow==wide && wide<1024);
    term_semantics(2);term_semantics(3);
    for (uint32_t mode=0;mode<4;++mode) term_resources(mode,all);
    puts("Independent substitutions, repeated diamonds, changing arguments, growth and partial failure passed");return 0;
}
