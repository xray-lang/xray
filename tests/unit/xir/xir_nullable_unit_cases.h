/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nullable_unit_cases.h - Tagged zero-payload data and exact receiving
 */
#ifndef XIR_NULLABLE_UNIT_CASES_H
#define XIR_NULLABLE_UNIT_CASES_H
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_array.h"

static void nullable_unit_presence(const XrXirValue *value,bool expected) {
    bool some=!expected;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if(expected) CHECK(payload && !payload->type && !payload->reserved && !payload->payload);
    else CHECK(!payload);
}
static void nullable_unit_compile_boundaries(const XrXirTypes *types) {
    ValueCompileProbe probe={types,NULL,(XrXirType)256,0,100};XrCompileResourceStats exact={0};
    size_t blocks=value_compile_live,bytes=value_compile_bytes,start=value_compile_calls;
    CHECK(value_compile_attempt(&probe,value_compile_limits(),&exact)==XR_XIR_OK);
    size_t sites=value_compile_calls-start;CHECK(sites>1 && exact.peak_bytes>1);
    for(size_t i=0;i<2;++i) {
        value_compile_fail_at=value_compile_calls+(i?sites-1:0);value_compile_injected=false;
        CHECK(value_compile_attempt(&probe,value_compile_limits(),NULL)==XR_XIR_OUT_OF_MEMORY);
        CHECK(value_compile_injected && value_compile_live==blocks && value_compile_bytes==bytes);
        value_compile_fail_at=SIZE_MAX;
    }
    XrCompileResourceLimits finite={XR_COMPILE_MEMORY_FINITE,exact.peak_bytes,false};
    CHECK(value_compile_attempt(&probe,finite,NULL)==XR_XIR_OK);
    --finite.memory_limit;CHECK(value_compile_attempt(&probe,finite,NULL)==XR_XIR_MEMORY_LIMIT);
    CHECK(value_compile_attempt(&probe,value_compile_limits(),NULL)==XR_XIR_OK);
    CHECK(value_compile_live==blocks && value_compile_bytes==bytes);
    printf("Nullable<Unit> compiler2 physical OOM and dynamic held peak%llu/minus1 passed\n",
        (unsigned long long)exact.peak_bytes);
}
static void nullable_unit_value_cases(void) {
    XrXirCallableParameter fields[]={{(XrXirType)256,0},{XR_XIR_UNIT,0}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256}
    };
    XrXirTypes types={nodes,4,NULL,NULL};nullable_unit_compile_boundaries(&types);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(value_compile_arena(&types,100,value_compile_limits(),&arena)==XR_XIR_VALUE_OK);
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(arena,(XrXirType)256);
    CHECK(layout && layout->value.size==1 && layout->value.alignment==1 && layout->tag_bytes==1);
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    uint64_t header_bytes=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,1048576};
    XrXirValue unit={0},none={0},some={0},outer={0},tuple={0},array={0},result={0};
    CHECK(!xr_xir_value_argument(&unit,arena,XR_XIR_UNIT));
    CHECK(xr_xir_value_admit(&unit,XR_XIR_UNIT,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_nullable_new((XrXirType)256,NULL,&admission,&none)==XR_XIR_VALUE_OK);
    uint64_t before=xr_xir_domain_stats(domain).live_bytes;
    CHECK(xr_xir_nullable_new((XrXirType)256,&unit,&admission,&some)==XR_XIR_VALUE_OK);
    uint64_t record_bytes=xr_xir_domain_stats(domain).live_bytes-before;CHECK(record_bytes>1);
    nullable_unit_presence(&none,false);nullable_unit_presence(&some,true);
    CHECK(xr_xir_value_admit(&some,(XrXirType)256,&admission)==XR_XIR_VALUE_OK);
    const XrXirValue malformed[]={{XR_XIR_UNIT,1,0},{XR_XIR_UNIT,0,1},{4,0,0},{XR_XIR_BOOL,0,0}};
    for(size_t i=0;i<sizeof(malformed)/sizeof(malformed[0]);++i) {
        CHECK(xr_xir_nullable_new((XrXirType)256,&malformed[i],&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!result.type && !result.reserved && !result.payload);
    }
    CHECK(xr_xir_nullable_new((XrXirType)257,&some,&admission,&outer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&outer,(XrXirType)257,&admission)==XR_XIR_VALUE_OK);
    XrXirValue pair[]={some,unit};
    CHECK(xr_xir_tuple_new((XrXirType)258,pair,2,&admission,&tuple)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&tuple,(XrXirType)258,&admission)==XR_XIR_VALUE_OK);
    XrXirValue elements[]={none,some};
    CHECK(xr_xir_array_new((XrXirType)259,elements,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&array,(XrXirType)259,&admission)==XR_XIR_VALUE_OK);
    for(int64_t i=0;i<2;++i) {
        XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(&array,i,&admission,&result,&fault)==XR_XIR_VALUE_OK);
        nullable_unit_presence(&result,i!=0);xr_xir_value_drop(&result);
    }
    before=xr_xir_domain_stats(domain).live_bytes;admission.work=0;
    CHECK(xr_xir_nullable_new((XrXirType)256,&unit,&admission,&result)==XR_XIR_VALUE_LIMIT && !result.type);
    CHECK(xr_xir_domain_stats(domain).live_bytes==before);admission.work=1000000;
    XrXirDomain *finite=NULL;CHECK(xr_xir_domain_new(header_bytes+record_bytes-1,&finite)==XR_XIR_VALUE_OK);
    XrXirValueAdmission limited={arena,finite,NULL,NULL,1000000,1048576};
    CHECK(xr_xir_nullable_new((XrXirType)256,&unit,&limited,&result)==XR_XIR_VALUE_LIMIT && !result.type);
    CHECK(xr_xir_domain_stats(finite).live_bytes==header_bytes);xr_xir_domain_drop(finite);finite=NULL;
    CHECK(xr_xir_domain_new(header_bytes+record_bytes,&finite)==XR_XIR_VALUE_OK);limited.domain=finite;
    CHECK(xr_xir_nullable_new((XrXirType)256,&unit,&limited,&result)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&result);CHECK(xr_xir_domain_stats(finite).live_bytes==header_bytes);xr_xir_domain_drop(finite);
    xr_xir_value_drop(&none);xr_xir_value_drop(&outer);xr_xir_value_drop(&tuple);xr_xir_value_drop(&array);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    nullable_unit_presence(&some,true);CHECK(xr_xir_value_copy(&some,&result)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&some);nullable_unit_presence(&result,true);xr_xir_value_drop(&result);
    CHECK(!value_compile_live && !value_compile_bytes);
    puts("Nullable<Unit> exact zero payload, packed Array, runtime limits and retired producer lifetime passed");
}
#endif // XIR_NULLABLE_UNIT_CASES_H
