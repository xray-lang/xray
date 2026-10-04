/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_equal_values.c - Independent typed equality and physical scratch gates
 */
#include "xir/xxir_equal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_equal_compile_owner.h"
#include "xir_runtime_allocations.h"

static XrXirValueAdmission equal_admission(XrXirDomain *domain, XrXirTypeArena *arena) {
    return (XrXirValueAdmission){arena, domain, NULL, NULL, 1000000, 1048576};
}
static void equal_scalars(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    static const XrXirType integer[] = {XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64,
        XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64};
    for (size_t t = 0; t < sizeof(integer)/sizeof(integer[0]); ++t) {
        uint32_t bits = xr_xir_integer_bits(integer[t]);
        int64_t low = xr_xir_integer_signed(integer[t]) ?
            (bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits-1))) : 0;
        int64_t high = bits == 64 ? (xr_xir_integer_signed(integer[t]) ? INT64_MAX : -1) :
            (INT64_C(1) << (bits-(xr_xir_integer_signed(integer[t]) ? 1 : 0)))-1;
        XrXirValue a = {(uint32_t)integer[t], 0, low}, b = a;
        XrXirValueAdmission admission = equal_admission(domain, NULL); bool result = false;
        CHECK(xr_xir_value_equal(&a, &b, integer[t], &admission, &result) == XR_XIR_VALUE_OK && result);
        b.payload = high;
        CHECK(xr_xir_value_equal(&a, &b, integer[t], &admission, &result) == XR_XIR_VALUE_OK && !result);
    }
    const XrXirValue a[] = {{XR_XIR_BOOL,0,1},{XR_XIR_F32,0,INT64_C(0x7fc00000)},
        {XR_XIR_F64,0,INT64_C(0x7ff8000000000000)},{XR_XIR_F32,0,0},{XR_XIR_F64,0,0}};
    const XrXirValue b[] = {{XR_XIR_BOOL,0,1},{XR_XIR_F32,0,INT64_C(0x7fc00000)},
        {XR_XIR_F64,0,INT64_C(0x7ff8000000000000)},{XR_XIR_F32,0,INT64_C(0x80000000)},
        {XR_XIR_F64,0,INT64_MIN}};
    const bool expected[] = {true,false,false,true,true};
    for (size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        XrXirValueAdmission admission=equal_admission(domain,NULL);bool result=!expected[i];
        CHECK(xr_xir_value_equal(&a[i],&b[i],(XrXirType)a[i].type,&admission,&result)==XR_XIR_VALUE_OK);
        CHECK(result==expected[i]);
    }
    XrXirValue malformed={XR_XIR_BOOL,0,2},good={XR_XIR_BOOL,0,1};
    XrXirValueAdmission admission=equal_admission(domain,NULL);bool result=true;
    CHECK(xr_xir_value_equal(&malformed,&good,XR_XIR_BOOL,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && result);
    XrXirValue unit={0};
    CHECK(xr_xir_value_equal(&unit,&unit,XR_XIR_UNIT,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && result);
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void equal_strings(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValue a={0},b={0},empty={0};
    CHECK(xr_xir_string_new(domain,"a\0b",3,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"a\0c",3,&b)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,NULL,0,&empty)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission=equal_admission(domain,NULL);bool result=true;
    CHECK(xr_xir_value_equal(&a,&b,XR_XIR_STRING,&admission,&result)==XR_XIR_VALUE_OK && !result);
    CHECK(xr_xir_value_equal(&empty,&empty,XR_XIR_STRING,&admission,&result)==XR_XIR_VALUE_OK && result);
    admission.work=4;result=false;
    CHECK(xr_xir_value_equal(&a,&a,XR_XIR_STRING,&admission,&result)==XR_XIR_VALUE_LIMIT && !result);
    admission.work=5;
    CHECK(xr_xir_value_equal(&a,&a,XR_XIR_STRING,&admission,&result)==XR_XIR_VALUE_OK && result && !admission.work);
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);xr_xir_value_drop(&empty);
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static XrXirTypeArena *equal_arena(const XrXirTypeNode *nodes, uint32_t count) {
    XrXirTypes types={nodes,count,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&equal_owner.context,&types,&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void equal_nan_backing(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_F64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_CONSTRUCTED_TYPE_BASE}};
    XrXirTypeArena *arena=equal_arena(nodes,2);
    XrXirValueAdmission admission=equal_admission(domain,arena);
    XrXirValue nan={XR_XIR_F64,0,INT64_C(0x7ff8000000000000)},array={0},copy={0},nested={0};
    CHECK(xr_xir_array_new(XR_XIR_CONSTRUCTED_TYPE_BASE,&nan,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array,&copy)==XR_XIR_VALUE_OK && array.payload==copy.payload);
    CHECK(xr_xir_array_new((XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),&array,1,&admission,&nested)==XR_XIR_VALUE_OK);
    bool result=true;admission.work=5;
    CHECK(xr_xir_value_equal(&array,&copy,XR_XIR_CONSTRUCTED_TYPE_BASE,&admission,&result)==XR_XIR_VALUE_OK && !result && !admission.work);
    result=true;admission.work=4;
    CHECK(xr_xir_value_equal(&array,&copy,XR_XIR_CONSTRUCTED_TYPE_BASE,&admission,&result)==XR_XIR_VALUE_LIMIT && result);
    admission.work=100;
    CHECK(xr_xir_value_equal(&nested,&nested,(XrXirType)nested.type,&admission,&result)==XR_XIR_VALUE_OK && !result);
    XrXirValue zero={XR_XIR_F64,0,0},negative={XR_XIR_F64,0,INT64_MIN},az={0},bz={0};
    CHECK(xr_xir_array_new(XR_XIR_CONSTRUCTED_TYPE_BASE,&zero,1,&admission,&az)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(XR_XIR_CONSTRUCTED_TYPE_BASE,&negative,1,&admission,&bz)==XR_XIR_VALUE_OK);
    CHECK(az.payload!=bz.payload);
    CHECK(xr_xir_value_equal(&az,&bz,XR_XIR_CONSTRUCTED_TYPE_BASE,&admission,&result)==XR_XIR_VALUE_OK && result);
    xr_xir_value_drop(&az);xr_xir_value_drop(&bz);xr_xir_value_drop(&nested);
    xr_xir_value_drop(&array);xr_xir_value_drop(&copy);xr_xir_compile_type_arena_drop(arena);
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void equal_deep_resource(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(16777216,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeNode nodes[257]={0};
    for (uint32_t i=0;i<257;++i) nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,
        .element=i ? (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i-1) : XR_XIR_I64};
    XrXirTypeArena *arena=equal_arena(nodes,257);
    XrXirValueAdmission admission=equal_admission(domain,arena);
    XrXirValue value={XR_XIR_I64,0,7};
    for (uint32_t i=0;i<257;++i) {
        XrXirValue next={0};
        CHECK(xr_xir_array_new((XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i),&value,1,&admission,&next)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&value);value=next;
    }
    size_t baseline=runtime_bytes,live=runtime_live;uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    admission=equal_admission(domain,arena);bool result=false;runtime_attempts=0;
    CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_OK && result);
    size_t sites=runtime_attempts;CHECK(sites==10 && admission.scratch_bytes==1048576);
    CHECK(runtime_bytes==baseline && runtime_live==live && xr_xir_domain_stats(domain).live_bytes==physical);
    for (uint32_t prior=0;prior<2;++prior) for (size_t i=0;i<sites;++i) {
        runtime_attempts=0;runtime_fail_at=i;admission=equal_admission(domain,arena);result=prior!=0;
        CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_OOM && result==(prior!=0));
        CHECK(runtime_bytes==baseline && runtime_live==live && admission.scratch_bytes==1048576);
        CHECK(xr_xir_domain_stats(domain).live_bytes==physical);runtime_fail_at=SIZE_MAX;
    }
    admission=equal_admission(domain,arena);admission.work=1037;result=false;
    CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_OK && result && !admission.work);
    admission=equal_admission(domain,arena);admission.work=1036;result=false;
    CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_LIMIT && !result);
    admission=equal_admission(domain,arena);admission.scratch_bytes=24575;result=false;
    CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_LIMIT && !result);
    CHECK(admission.scratch_bytes==24575 && runtime_bytes==baseline && runtime_live==live);
    admission=equal_admission(domain,arena);admission.scratch_bytes=24576;
    CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==XR_XIR_VALUE_OK && result);
    CHECK(admission.scratch_bytes==24576 && runtime_bytes==baseline && runtime_live==live);
    for (uint32_t minus=0;minus<2;++minus) {
        XrXirDomain *scratch=NULL;
        CHECK(xr_xir_domain_new(sizeof(XrXirDomain)+24576-minus,&scratch)==XR_XIR_VALUE_OK);
        uint64_t retained=xr_xir_domain_stats(scratch).live_bytes;
        admission=equal_admission(scratch,arena);result=false;
        CHECK(xr_xir_value_equal(&value,&value,(XrXirType)value.type,&admission,&result)==
            (minus ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
        CHECK(result==!minus && admission.scratch_bytes==1048576 &&
            xr_xir_domain_stats(scratch).live_bytes==retained);
        xr_xir_domain_drop(scratch);CHECK(runtime_bytes==baseline && runtime_live==live);
    }
    printf("typed equality depth257, actualOOM=%zu, work1037/1036, scratch/domain24576/24575 PASS\n",sites);
    xr_xir_value_drop(&value);xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    CHECK(!runtime_live && !runtime_bytes);
}
static void equal_arena_authority(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    XrXirTypeArena *actual=equal_arena(&node,1),*foreign=equal_arena(&node,1);
    CHECK(actual!=foreign);
    XrXirValueAdmission admission=equal_admission(domain,actual);
    XrXirValue leaf={XR_XIR_I64,0,7},array={0};
    CHECK(xr_xir_array_new(XR_XIR_CONSTRUCTED_TYPE_BASE,&leaf,1,&admission,&array)==XR_XIR_VALUE_OK);
    XrXirValue original=array;size_t live=runtime_live,bytes=runtime_bytes;
    admission=equal_admission(domain,foreign);bool result=true;runtime_attempts=0;
    CHECK(xr_xir_value_equal(&array,&array,(XrXirType)array.type,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(result && !runtime_attempts && !memcmp(&array,&original,sizeof(array)) && runtime_live==live && runtime_bytes==bytes);
    admission=equal_admission(domain,NULL);
    CHECK(xr_xir_value_equal(&array,&array,(XrXirType)array.type,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && result);
    CHECK(xr_xir_value_equal(&array,&array,XR_XIR_I64,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && result);
    xr_xir_value_drop(&array);xr_xir_compile_type_arena_drop(actual);xr_xir_compile_type_arena_drop(foreign);
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void equal_excluded_carriers(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    XrXirTypeArena *arena=equal_arena(&node,1);
    XrXirValueAdmission admission=equal_admission(domain,arena);
    XrXirValue values[3]={{0}},integer={XR_XIR_I64,0,7};
    CHECK(xr_xir_atomic_i64_new(domain,7,&values[0])==XR_XIR_VALUE_OK);
    XrXirPanicPayload panic={{445,0,0,0},{0}};
    CHECK(xr_xir_string_new(domain,NULL,0,&panic.message)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_panic_info_new(domain,&panic,&values[1])==XR_XIR_VALUE_OK);xr_xir_panic_drop(&panic);
    CHECK(xr_xir_cell_new(domain,arena,XR_XIR_CONSTRUCTED_TYPE_BASE,&integer,&admission,&values[2])==XR_XIR_VALUE_OK);
    for (uint32_t i=0;i<3;++i) {
        CHECK(xr_xir_value_valid(&values[i]));XrXirValue original=values[i];
        size_t live=runtime_live,bytes=runtime_bytes;bool result=true;runtime_attempts=0;
        CHECK(xr_xir_value_equal(&values[i],&values[i],(XrXirType)values[i].type,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(result && !runtime_attempts && !memcmp(&original,&values[i],sizeof(original)) && runtime_live==live && runtime_bytes==bytes);
        xr_xir_value_drop(&values[i]);
    }
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    source_fixture_owner_new(&equal_owner);
    equal_scalars();equal_strings();equal_nan_backing();equal_deep_resource();equal_arena_authority();
    equal_excluded_carriers();
    source_fixture_owner_free(&equal_owner);
    CHECK(!equal_compile_live && !equal_compile_bytes && !equal_compile_records);
    puts("typed equality scalar/STRING/shared-backing IEEE/physical PASS");return 0;
}
