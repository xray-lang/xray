/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_values.c - Ordered owners, backing authority and physical release
 */
#include "xir/xxir_tuple.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_class.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
static XrXirType constructed(uint32_t n) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);}
static XrXirCompileContext context_new(void) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrXirValueAdmission admission(XrXirTypeArena *arena,XrXirDomain *domain) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,1000000,262144};
}
static void check_string(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
}
static void ordered_owners(void) {
    XrXirCompileContext context=context_new();
    XrXirCallableParameter first[]={{XR_XIR_I64,0},{XR_XIR_UNIT,0},{XR_XIR_STRING,0},{XR_XIR_UNIT,0}};
    XrXirCallableParameter second[]={{constructed(0),0},{XR_XIR_BOOL,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=first,.parameter_count=4},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=second,.parameter_count=2}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *a=NULL,*b=NULL;CHECK(xr_xir_domain_new(1048576,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&b)==XR_XIR_VALUE_OK);
    XrXirValue fields[4]={{XR_XIR_I64,0,73},{0},{0},{0}},inner={0},outer={0},copy={0};
    CHECK(xr_xir_string_new(b,"ordered-owner",13,&fields[2])==XR_XIR_VALUE_OK);
    XrXirValueAdmission admit=admission(arena,a);
    CHECK(xr_xir_tuple_new(constructed(0),fields,4,&admit,&inner)==XR_XIR_VALUE_OK);
    XrXirValue nested[2]={inner,{XR_XIR_BOOL,0,1}};
    CHECK(xr_xir_tuple_new(constructed(1),nested,2,&admit,&outer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&outer,&copy)==XR_XIR_VALUE_OK && copy.payload==outer.payload);
    CHECK(xr_xir_value_admit(&outer,constructed(1),&admit)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&fields[1],XR_XIR_UNIT,&admit)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&fields[2]);xr_xir_value_drop(&inner);xr_xir_value_drop(&outer);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(a);xr_xir_domain_drop(b);
    XrXirValue selected={0},text={0},unit={0},integer={0};
    CHECK(xr_xir_tuple_get(&copy,0,&selected)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&selected,0,&integer)==XR_XIR_VALUE_OK && integer.type==XR_XIR_I64 && integer.payload==73);
    CHECK(xr_xir_tuple_get(&selected,1,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.reserved && !unit.payload);
    CHECK(xr_xir_tuple_get(&selected,2,&text)==XR_XIR_VALUE_OK);check_string(&text,"ordered-owner");
    CHECK(xr_xir_tuple_get(&selected,3,&unit)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&selected,4,&unit)==XR_XIR_VALUE_BAD_ARGUMENT && !unit.payload);
    CHECK(xr_xir_tuple_get(&selected,0,&text)==XR_XIR_VALUE_BAD_ARGUMENT);check_string(&text,"ordered-owner");
    xr_xir_value_drop(&copy);xr_xir_value_drop(&selected);check_string(&text,"ordered-owner");xr_xir_value_drop(&text);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    puts("Tuple n4 Unit interleave, nested owner, occupied output and two-domain escape physical0 PASS");
}
typedef struct Gate {bool allowed;size_t checks,releases;} Gate;
static void release_gate(void *owner) {++((Gate *)owner)->releases;}
static XrXirValueStatus gate_admit(void *owner,const XrXirFunctionBinding *binding,XrXirType type,uint64_t *work) {
    Gate *gate=owner;++gate->checks;
    if (!*work) return XR_XIR_VALUE_LIMIT;
    --*work;
    return gate->allowed && binding->owner==gate && type==constructed(0) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void class_backing_authority(void) {
    XrXirCompileContext context=context_new();
    XrXirCallableParameter fields[]={{XR_XIR_UNIT,0},{constructed(0),0}};
    XrXirType class_field=constructed(3);
    XrXirNominalFieldIdentity identity_field={{"items",5},XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity={.module={"root",4},.name={"Box",3},.kind=XR_XIR_NOMINAL_CLASS,
        .fields=&identity_field,.field_count=1,.flags=XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable nominals={NULL,1,&identity};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=constructed(1)},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(2)},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=&class_field,.field_count=1}}};
    XrXirTypes types={nodes,5,&nominals,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_class_field_verify(&context,&types,constructed(1))==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_class_field_verify(&context,&types,constructed(2))==XR_XIR_OK);
    XrXirDomain *domain=NULL,*other=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&other)==XR_XIR_VALUE_OK);
    Gate gate={true,0,0};XrXirValueAdmission admit=admission(arena,domain);
    admit.function=gate_admit;admit.context=&gate;
    XrXirValue fn={0},tuple={0},array={0},nullable={0},box={0};
    XrXirFunctionBinding binding={&gate,release_gate,0,NULL,0};
    CHECK(xr_xir_function_new(domain,arena,constructed(0),&binding,&admit,&fn)==XR_XIR_VALUE_OK);
    XrXirValue values[2]={{0},fn};
    CHECK(xr_xir_tuple_new(constructed(1),values,2,&admit,&tuple)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(constructed(2),&tuple,1,&admit,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(constructed(3),&array,&admit,&nullable)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new(constructed(4),&nullable,1,&admit,&box)==XR_XIR_VALUE_OK);
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes,walk_sites=0;
    for (size_t pass=0;pass<=walk_sites;++pass) {
        XrXirValueAdmission walk=admission(arena,domain);walk.function=gate_admit;walk.context=&gate;
        runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirValueStatus status=xr_xir_value_admit(&box,constructed(4),&walk);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if (!pass) {CHECK(status==XR_XIR_VALUE_OK);walk_sites=attempts;CHECK(walk_sites>=3);}
        else CHECK(status==XR_XIR_VALUE_OOM && attempts>=pass);
        CHECK(walk.scratch_bytes==262144 && runtime_live==baseline_live && runtime_bytes==baseline_bytes);
    }
    XrXirValueAdmission no_scratch=admit;no_scratch.scratch_bytes=0;
    CHECK(xr_xir_value_admit(&box,constructed(4),&no_scratch)==XR_XIR_VALUE_LIMIT && !no_scratch.scratch_bytes);
    XrXirValueAdmission no_work=admit;no_work.work=1;
    CHECK(xr_xir_value_admit(&box,constructed(4),&no_work)==XR_XIR_VALUE_LIMIT);
    size_t checks=gate.checks;CHECK(xr_xir_value_admit(&box,constructed(4),&admit)==XR_XIR_VALUE_OK && gate.checks>checks);
    gate.allowed=false;XrXirValue candidate={0};checks=gate.checks;
    CHECK(xr_xir_value_admit(&box,constructed(4),&admit)==XR_XIR_VALUE_BAD_ARGUMENT && gate.checks>checks);
    CHECK(xr_xir_class_set(&box,0,&nullable,&admit)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_new(constructed(4),&nullable,1,&admit,&candidate)==XR_XIR_VALUE_BAD_ARGUMENT && !candidate.type);
    XrXirValueAdmission foreign=admit;foreign.domain=other;
    CHECK(xr_xir_value_admit(&box,constructed(4),&foreign)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue data={0},copy={0};XrXirValueAdmission read={arena,domain,NULL,NULL,1000000,262144};
    CHECK(xr_xir_class_get(&box,0,&read,&data)==XR_XIR_VALUE_OK && xr_xir_value_valid(&data));
    CHECK(xr_xir_tuple_get(&tuple,1,&copy)==XR_XIR_VALUE_OK && xr_xir_value_valid(&copy));
    CHECK(xr_xir_value_admit(&copy,constructed(0),&admit)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&copy);xr_xir_value_drop(&data);xr_xir_value_drop(&box);xr_xir_value_drop(&nullable);
    xr_xir_value_drop(&array);xr_xir_value_drop(&tuple);xr_xir_value_drop(&fn);CHECK(gate.releases==1);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_domain_drop(other);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    printf("real class/Nullable/Array/Tuple backing gate, walk faults=%zu, pure data reads physical0 PASS\n",walk_sites);
}
static void actual_faults(void) {
    XrXirCompileContext context=context_new();
    XrXirCallableParameter fields[]={{XR_XIR_UNIT,0},{XR_XIR_STRING,0},{XR_XIR_I64,0},{XR_XIR_STRING,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=4};
    XrXirTypes types={&node,1,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValue values[4]={{0},{0},{XR_XIR_I64,0,91},{0}};
    CHECK(xr_xir_string_new(domain,"fault-owner",11,&values[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"last-owner",10,&values[3])==XR_XIR_VALUE_OK);
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes,sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirValue output={0};XrXirValueAdmission admit=admission(arena,domain);
        runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirValueStatus status=xr_xir_tuple_new(constructed(0),values,4,&admit,&output);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if (!pass) {CHECK(status==XR_XIR_VALUE_OK);sites=attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_VALUE_OOM && !output.type && attempts>=pass);
        xr_xir_value_drop(&output);CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);
        check_string(&values[1],"fault-owner");CHECK(admit.scratch_bytes==262144);
    }
    XirObject *first=object_pointer(&values[1]),*last=object_pointer(&values[3]);
    uint32_t first_refs=atomic_load_explicit(&first->references,memory_order_relaxed);
    atomic_store_explicit(&last->references,UINT32_MAX,memory_order_relaxed);
    XrXirValue partial={0};XrXirValueAdmission partial_admit=admission(arena,domain);
    CHECK(xr_xir_tuple_new(constructed(0),values,4,&partial_admit,&partial)==XR_XIR_VALUE_REFCOUNT_LIMIT && !partial.type);
    CHECK(atomic_load_explicit(&first->references,memory_order_relaxed)==first_refs);
    atomic_store_explicit(&last->references,1,memory_order_relaxed);
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);check_string(&values[3],"last-owner");
    XrXirValue out={0};XrXirValueAdmission limit=admission(arena,domain);limit.work=4;
    CHECK(xr_xir_tuple_new(constructed(0),values,4,&limit,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    values[0].payload=1;limit=admission(arena,domain);
    CHECK(xr_xir_tuple_new(constructed(0),values,4,&limit,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);values[0].payload=0;
    xr_xir_value_drop(&values[1]);xr_xir_value_drop(&values[3]);xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    printf("Tuple actual construction faults=%zu, inputs/out/scratch preserved, final physical0 PASS\n",sites);
}
static XrXirValueStatus arena_attempt(XrCompileResourceLimits limits,size_t fault,
    XrCompileResourceStats *stats,size_t *attempts) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCallableParameter fields[]={{XR_XIR_UNIT,0},{XR_XIR_STRING,0},{XR_XIR_I64,0}};
    XrXirCallableParameter nested[]={{constructed(0),0},{XR_XIR_BOOL,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=nested,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=constructed(1)}};
    XrXirTypes types={nodes,3,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    XrXirValue values[]={{0},{0},{XR_XIR_I64,0,19}},tuple={0};
    runtime_attempts=0;runtime_fail_at=fault;
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&context,&types,&arena);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_domain_new(1048576,&domain);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_string_new(domain,"ledger",6,&values[1]);
    if (status==XR_XIR_VALUE_OK) {
        XrXirValueAdmission admit=admission(arena,domain);
        status=xr_xir_tuple_new(constructed(0),values,3,&admit,&tuple);
        if (status==XR_XIR_VALUE_OK) {XrXirValue got={0};CHECK(xr_xir_tuple_get(&tuple,1,&got)==XR_XIR_VALUE_OK);
            check_string(&got,"ledger");xr_xir_value_drop(&got);}
    }
    *attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
    xr_xir_value_drop(&tuple);xr_xir_value_drop(&values[1]);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);return status;
}
static void arena_ledger(void) {
    XrCompileResourceLimits generous={8388608,4194304,128000000};XrCompileResourceStats required={0};size_t sites=0;
    CHECK(arena_attempt(generous,SIZE_MAX,&required,&sites)==XR_XIR_VALUE_OK && sites>=3);
    for (size_t fault=0;fault<sites;++fault) {XrCompileResourceStats stats={0};size_t attempts=0;
        CHECK(arena_attempt(generous,fault,&stats,&attempts)==XR_XIR_VALUE_OOM && attempts>fault);}
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=generous;
        if (!axis) limits.allocated_bytes=required.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=required.peak_bytes-minus;
        else limits.work=required.work-minus;
        XrCompileResourceStats stats={0};size_t attempts=0;
        CHECK(arena_attempt(limits,SIZE_MAX,&stats,&attempts)==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(stats.allocated_bytes<=limits.allocated_bytes && stats.peak_bytes<=limits.live_bytes && stats.work<=limits.work);
    }
    printf("Tuple actual arena/runtime faults=%zu, one finite ledger allocated/live/work exact and minus-one physical0 PASS\n",sites);
}
int main(void) {ordered_owners();class_backing_authority();actual_faults();arena_ledger();return 0;}
