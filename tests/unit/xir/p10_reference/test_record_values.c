/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_record_values.c - Exact records, snapshots and failure-atomic replacement
 */
#include "xir/xxir_record.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
static XrXirValueAdmission admit_new(XrXirTypeArena *arena,XrXirDomain *domain) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,1000000,262144};
}
static void expect_text(const XrXirValue *record,XrXirValueAdmission *admit,const char *expected) {
    XrXirValue value={0};const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_record_get(record,1,admit,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&value);
}
static void record_values(void) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirLiteral names[]={{"empty",5},{"name",4},{"size",4}};
    XrXirCallableParameter fields[]={{XR_XIR_UNIT,0},{XR_XIR_STRING,0},{XR_XIR_I64,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_RECORD,.parameters=fields,.parameter_count=3,.record={names}},
        {.kind=XR_XIR_TYPE_RECORD,.record={NULL}},{.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}};
    XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admit=admit_new(arena,domain);XrXirValue values[]={{0},{0},{XR_XIR_I64,0,7}};
    CHECK(xr_xir_string_new(domain,"first",5,&values[1])==XR_XIR_VALUE_OK);
    XrXirValue record={0},snapshot={0},empty={0},tuple={0},selected={0},next={0};
    CHECK(xr_xir_record_new((XrXirType)256,values,3,&admit,&record)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_new((XrXirType)257,NULL,0,&admit,&empty)==XR_XIR_VALUE_OK && empty.type==257 && empty.payload);
    CHECK(xr_xir_value_valid(&empty) && xr_xir_value_admit(&empty,(XrXirType)257,&admit)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_get(&empty,0,&admit,&selected)==XR_XIR_VALUE_BAD_ARGUMENT && !selected.type);
    CHECK(xr_xir_tuple_get(&record,1,&selected)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_tuple_new((XrXirType)256,values,3,&admit,&tuple)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_record_new((XrXirType)258,values,3,&admit,&tuple)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_tuple_new((XrXirType)258,values,3,&admit,&tuple)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_get(&tuple,1,&admit,&selected)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_record_get(&record,0,&admit,&selected)==XR_XIR_VALUE_OK && !selected.type && !selected.payload);
    CHECK(xr_xir_value_copy(&record,&snapshot)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"second",6,&next)==XR_XIR_VALUE_OK);
    XrXirValue cell={0},stored={0},number={XR_XIR_I64,0,17};XrXirValuePlace cell_place={0};
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)259,&record,&admit,&cell)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_value_place(&cell,&admit,&cell_place)==XR_XIR_VALUE_OK && cell_place.type==(XrXirType)256);
    CHECK(xr_xir_record_set(&cell_place,2,&number,&admit)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&cell,&stored)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_get(&stored,2,&admit,&selected)==XR_XIR_VALUE_OK && selected.payload==17);
    xr_xir_value_drop(&selected);xr_xir_value_drop(&stored);xr_xir_value_drop(&cell);
    CHECK(xr_xir_record_get(&record,2,&admit,&selected)==XR_XIR_VALUE_OK && selected.payload==7);
    xr_xir_value_drop(&selected);
    XrXirValuePlace place={(XrXirType)256,&record.payload};
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes,sites=0;uint64_t work=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirValue current={0};CHECK(xr_xir_value_copy(&record,&current)==XR_XIR_VALUE_OK);
        XrXirValuePlace current_place={(XrXirType)256,&current.payload};
        XrXirValueAdmission attempt=admit_new(arena,domain);runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirValueStatus status=xr_xir_record_set(&current_place,1,&next,&attempt);
        size_t calls=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if (!pass) {CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites);work=1000000-attempt.work;expect_text(&current,&admit,"second");}
        else {CHECK(status==XR_XIR_VALUE_OOM && current.payload==record.payload && calls>=pass);expect_text(&current,&admit,"first");}
        CHECK(attempt.scratch_bytes==262144);xr_xir_value_drop(&current);
        CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);expect_text(&snapshot,&admit,"first");
    }
    for (uint64_t prefix=0;prefix<work;++prefix) {
        XrXirValueAdmission attempt=admit_new(arena,domain);attempt.work=prefix;
        CHECK(xr_xir_record_set(&place,1,&next,&attempt)==XR_XIR_VALUE_LIMIT && record.payload==snapshot.payload);
        CHECK(attempt.scratch_bytes==262144 && runtime_live==baseline_live && runtime_bytes==baseline_bytes);
    }
    XrXirValueAdmission exact=admit_new(arena,domain);exact.work=work;
    CHECK(xr_xir_record_set(&place,1,&next,&exact)==XR_XIR_VALUE_OK && !exact.work && record.payload!=snapshot.payload);
    expect_text(&record,&admit,"second");expect_text(&snapshot,&admit,"first");
    XrXirValue bad_unit={XR_XIR_UNIT,0,1};int64_t stable=record.payload;
    CHECK(xr_xir_record_set(&place,0,&bad_unit,&admit)==XR_XIR_VALUE_BAD_ARGUMENT && record.payload==stable);
    CHECK(xr_xir_record_set(&place,3,&next,&admit)==XR_XIR_VALUE_BAD_ARGUMENT && record.payload==stable);
    XirObject *old=object_pointer(&next);uint32_t references=atomic_load_explicit(&old->references,memory_order_relaxed);
    atomic_store_explicit(&old->references,UINT32_MAX,memory_order_relaxed);
    CHECK(xr_xir_record_set(&place,1,&values[1],&admit)==XR_XIR_VALUE_OK);
    atomic_store_explicit(&old->references,references-1,memory_order_relaxed);expect_text(&record,&admit,"first");
    xr_xir_value_drop(&record);xr_xir_value_drop(&snapshot);xr_xir_value_drop(&empty);xr_xir_value_drop(&tuple);
    xr_xir_value_drop(&next);xr_xir_value_drop(&values[1]);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Record exact/empty/Unit identity,snapshot replacement,%zu allocation failures,all work prefixes0..%llu,physical0/0 PASS\n",
        sites,(unsigned long long)(work-1));
}
#include "record_receiving_resources.h"
int main(void) {record_values();record_receiving_resources();return 0;}
