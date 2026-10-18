/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_decimal_parse_values.c - Owned decimal receiving and exact work limits
 */
#include "xir/xxir_nullable.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static XrXirValueAdmission decimal_value_admission(XrXirDomain *domain,XrXirTypeArena *arena,uint64_t work) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,work,1048576};
}
static void decimal_value_expect(const XrXirValue *value,bool expected_some,uint64_t bits) {
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected_some);
    if(some)CHECK(payload && payload->type==XR_XIR_F64 && !payload->reserved && (uint64_t)payload->payload==bits);
    else CHECK(!payload);
}
static void decimal_value_case(XrXirDomain *domain,XrXirTypeArena *arena,const char *bytes,
    bool some,uint64_t bits,bool prefixes) {
    XrXirValue text={0},output={0};CHECK(xr_xir_string_new(domain,bytes,strlen(bytes),&text)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission=decimal_value_admission(domain,arena,UINT64_MAX);
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;runtime_attempts=0;
    CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output)==XR_XIR_VALUE_OK);
    uint64_t needed=UINT64_MAX-admission.work;size_t sites=runtime_attempts;CHECK(needed>sizeof(text) && sites>0);
    decimal_value_expect(&output,some,bits);xr_xir_value_drop(&output);
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);
    for(size_t at=0;at<sites;++at) {
        admission=decimal_value_admission(domain,arena,UINT64_MAX);runtime_attempts=0;runtime_fail_at=at;
        XrXirValueStatus status=xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output);runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_VALUE_OOM && !output.type && !output.reserved && !output.payload);
        CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);
        admission=decimal_value_admission(domain,arena,UINT64_MAX);
        CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output)==XR_XIR_VALUE_OK);
        decimal_value_expect(&output,some,bits);xr_xir_value_drop(&output);
    }
    if(prefixes)for(uint64_t limit=0;limit<=needed;++limit) {
        admission=decimal_value_admission(domain,arena,limit);
        XrXirValueStatus status=xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output);
        CHECK(admission.work<=limit);
        if(limit==needed) {CHECK(status==XR_XIR_VALUE_OK && !admission.work);decimal_value_expect(&output,some,bits);xr_xir_value_drop(&output);}
        else CHECK(status==XR_XIR_VALUE_LIMIT && !output.type && !output.reserved && !output.payload);
        CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);
    }
    XrXirValue sentinel={XR_XIR_I64,0,42};admission=decimal_value_admission(domain,arena,UINT64_MAX);
    CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&sentinel)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(sentinel.type==XR_XIR_I64 && !sentinel.reserved && sentinel.payload==42 && admission.work==UINT64_MAX);
    printf("Decimal Value some=%u exact_work=%llu all_prefixes=%u OOM=%zu original-owner retry/physical refund\n",
        some,(unsigned long long)needed,prefixes,sites);
    xr_xir_value_drop(&text);
}
static void decimal_shared_budget(const XrXirCompileContext *context,XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL;XrXirValue text={0},output={0};const char *bytes="1.7976931348623157e308";
    CHECK(xr_xir_compile_domain_new(context->resources,1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,bytes,strlen(bytes),&text)==XR_XIR_VALUE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirValueAdmission admission=decimal_value_admission(domain,arena,UINT64_MAX);
    CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output)==XR_XIR_VALUE_OK);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.work-before.work>=55000);
    decimal_value_expect(&output,true,UINT64_C(0x7fefffffffffffff));xr_xir_value_drop(&output);xr_xir_value_drop(&text);xr_xir_domain_drop(domain);
    const XrXirDomainBudgetControls limits={1048576,1048576,1024,1048576,1048576};domain=NULL;
    CHECK(xr_xir_domain_new_budgeted(1048576,&limits,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,bytes,strlen(bytes),&text)==XR_XIR_VALUE_OK);
    admission=decimal_value_admission(domain,arena,UINT64_MAX);
    CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&output)==XR_XIR_VALUE_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && xr_xir_domain_budget_stats(domain).work<=1024);
    xr_xir_value_drop(&text);xr_xir_domain_drop(domain);
    puts("Decimal kernel charges the original compiler ledger and cannot turn runtime Domain work exhaustion into None");
}
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_F64};XrXirTypes types={&node,1,NULL,NULL};
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    static const struct {const char *text;bool some;uint64_t bits;} cases[]={
        {"1.7976931348623157e308",true,UINT64_C(0x7fefffffffffffff)},
        {"1.7976931348623158e308",true,UINT64_C(0x7fefffffffffffff)},
        {"1.00000000000000011102230246251565404236316680908203125",true,UINT64_C(0x3ff0000000000000)},
        {"  -0\t",true,UINT64_C(0x8000000000000000)},
        {"4.9406564584124654e-324",true,UINT64_C(1)},
        {"1e9999999999",true,UINT64_C(0x7ff0000000000000)},
        {"-1e-9999999999",true,UINT64_C(0x8000000000000000)},
        {"1e9999999999x",false,0},{"1_0",false,0},{"nan",false,0},{"",false,0},{" ",false,0}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i)
        decimal_value_case(domain,arena,cases[i].text,cases[i].some,cases[i].bits,i==0 || i==7);
    decimal_shared_budget(&context,arena);
    XrXirValueAdmission admission=decimal_value_admission(domain,arena,0);XrXirValue output={0};
    CHECK(xr_xir_f64_try_parse((const XrXirValue *)(uintptr_t)1,(XrXirType)256,&admission,&output)==XR_XIR_VALUE_LIMIT);
    XrXirValue text={0},kept={0};CHECK(xr_xir_string_new(domain,"-0",2,&text)==XR_XIR_VALUE_OK);
    admission=decimal_value_admission(domain,arena,UINT64_MAX);
    CHECK(xr_xir_f64_try_parse(&text,(XrXirType)256,&admission,&kept)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text);xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_compile_resources_release(context.resources);
    decimal_value_expect(&kept,true,UINT64_C(0x8000000000000000));xr_xir_value_drop(&kept);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    puts("Decimal exact Some/None outlives arena/Domain/compiler ledger; all physical owners released");return 0;
}
