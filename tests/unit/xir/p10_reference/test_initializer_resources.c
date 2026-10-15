/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_initializer_resources.c - Complete initializer Source allocation faults
 *
 * KEY CONCEPT:
 *   Every real compiler allocation may fail without publishing partial Source.
 *   Cumulative allocation, peak live and work bounds stay independent.
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[20000];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t i = 0; while (i < 20000 && allocations[i].pointer) ++i;
    CHECK(i < 20000); allocations[i] = (Allocation){memory, bytes}; physical += bytes;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 20000 && allocations[i].pointer != memory) ++i;
    CHECK(i < 20000); physical -= allocations[i].bytes; allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_alloc(n)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

static const char source[] =
    "fn init()->i64{return 7}\n"
    "struct Box{value:i64 init(value){this.value=value} get()->i64{return this.value}}\n"
    "export fn referenceAnswer()->i64{const init=3;return Box(41).get()+init}\n";
static const char array_source[] =
    "fn seed()->Array<i8>{var xs:Array<i8>=[7,11,13];return xs.reserve(8)}\n"
    "struct Batch{items:Array<i8>=comptime seed();last:i8;init(last){this.last=last}}\n"
    "struct Holder<T>{value:T;init(value){this.value=value}}\n"
    "export fn referenceAnswer()->i64{const first=Batch(17);const second=Batch(19);"
    "const held=Holder<Array<i8>>(first.items);var changed=held.value;changed.push(first.last);"
    "return (changed[0] as i64)+(changed[1] as i64)+(changed[2] as i64)+(changed[3] as i64)"
    "+(second.last as i64)+held.value.capacity*100+len(second.items)*1000}\n";
static const char optional_tuple_source[] =
    "fn layer<T>(value:T)->T?{return value}\n"
    "fn identity<T>(value:T)->T{return value}\n"
    "struct Cached{value:(i8,string)=comptime identity<(i8,string)>((7,\"a\\0b\"));"
    "state:i8?" "?=comptime layer<i8?>(null);init(){}}\n"
    "fn classify(value:i8?" "?)->i64{if(value==null){return 0};if(value==null){return 1};return 2}\n"
    "export fn referenceAnswer()->i64{const cached=Cached();"
    "return classify(cached.state)*100+(cached.value.0 as i64)*10+len(cached.value.1)}\n";
static const char field_place_source[] =
    "struct Inner{items:Array<i64>}\n"
    "struct Batch{inner:Inner=Inner{items:[7,11]};init(last:i64){this.inner.items.push(last);this.inner.items[0]=13}}\n"
    "class ObjectBatch{items:Array<i64>=[17];init(last:i64){this.items.push(last)}}\n"
    "struct Generic<T>{items:Array<T>;init(first:T,last:T){this.items=[first];this.items.push(last)}}\n"
    "export fn referenceAnswer()->i64{const a=Batch(19);const b=Batch(23);const c=ObjectBatch(29);const g=Generic<i64>(31,37);"
    "return a.inner.items[0]*10000+a.inner.items[1]*100+a.inner.items[2]+b.inner.items[2]+c.items[0]+c.items[1]+g.items[0]+g.items[1]}\n";
static const char struct_spread_order_source[] =
    "var trace: i64 = 0\n"
    "struct Pair {\n"
    "    x: i64\n"
    "    y: i64 = 30\n"
    "}\n"
    "struct Head { x: i64 }\n"
    "struct Box<T> { value: T }\n"
    "fn base() -> Pair {\n"
    "    trace = trace * 10 + 1\n"
    "    return Pair{x: 10, y: 20}\n"
    "}\n"
    "fn next() -> i64 {\n"
    "    trace = trace * 10 + 2\n"
    "    return 40\n"
    "}\n"
    "fn other() -> Head {\n"
    "    trace = trace * 10 + 3\n"
    "    const x = 50\n"
    "    return Head{x}\n"
    "}\n"
    "export fn referenceAnswer() -> i64 {\n"
    "    trace = 0\n"
    "    const pair = Pair{...base(), y: next(), ...other()}\n"
    "    const boxed = Box<i64>{...Box<i64>{value: 7}}\n"
    "    var copied = Pair{...pair}\n"
    "    copied.x = 8\n"
    "    return pair.x * 1000000 + pair.y * 10000 + trace * 10 + boxed.value + copied.x\n"
    "}\n";
static const char struct_spread_cow_source[] =
    "struct Batch<T> {\n"
    "    items: Array<T>\n"
    "    marker: i64 = 5\n"
    "}\n"
    "export fn referenceAnswer() -> i64 {\n"
    "    const original = Batch<i64>{items: [3, 7]}\n"
    "    var copied = Batch<i64>{...original}\n"
    "    copied.items[0] = 11\n"
    "    copied.items.push(19)\n"
    "    return original.items[0] * 1000 + copied.items[0] * 100 + len(copied.items) * 10 + copied.marker\n"
    "}\n";
typedef struct InitializerCase { const char *name, *text;size_t length; } InitializerCase;
static const InitializerCase cases[] = {
    {"ordinary",source,sizeof(source)-1},
    {"comptime-array",array_source,sizeof(array_source)-1},
    {"comptime-optional-tuple",optional_tuple_source,sizeof(optional_tuple_source)-1},
    {"field-places",field_place_source,sizeof(field_place_source)-1},
    {"struct-spread-order",struct_spread_order_source,sizeof(struct_spread_order_source)-1},
    {"struct-spread-cow",struct_spread_cow_source,sizeof(struct_spread_cow_source)-1}
};
static const XrCompileResourceLimits limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};

static XrXirStatus pipeline(const InitializerCase *fixture,
    const XrCompileResourceLimits *bounds, XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrCompileResourceStatus opened = xr_compile_resources_new(bounds, &resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources && !physical);
        return opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrXirStatus status = XR_XIR_OK;
    XrCompilerSessionStatus session_status = xr_compile_session_new(resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "initializer-fi", NULL};
    const XrXirSourceRequest request = {session, NULL, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    const XrXirSourceText text = {NULL, fixture->text, fixture->length};
    status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, NULL);
    CHECK(diagnostic.status == status);
    if (status == XR_XIR_OK) CHECK(result.checked && result.snapshot);
    else CHECK(!result.checked && !result.snapshot);
done:
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources); CHECK(!physical);
    return status;
}

static void initializer_case(const InitializerCase *fixture, bool baseline) {
    XrCompileResourceStats normal = {0}, observed = {0};
    attempts=0;fail_at=SIZE_MAX;
    CHECK(pipeline(fixture,&limits, &normal) == XR_XIR_OK);
    size_t sites = attempts; CHECK(sites && sites < 20000);
    printf("initializer %s source sites=%zu allocated=%llu peak=%llu work=%llu physical=0\n", fixture->name,sites,
        (unsigned long long)normal.allocated_bytes, (unsigned long long)normal.peak_bytes,
        (unsigned long long)normal.work);
    fflush(stdout);
    if (baseline) return;
    for (size_t i = 0; i < sites; ++i) {
        attempts = 0; fail_at = i;
        CHECK(pipeline(fixture,&limits, &observed) == XR_XIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    XrCompileResourceLimits exact = {normal.allocated_bytes, normal.peak_bytes, normal.work};
    attempts = 0; CHECK(pipeline(fixture,&exact, &observed) == XR_XIR_OK);
    CHECK(observed.allocated_bytes == normal.allocated_bytes && observed.peak_bytes == normal.peak_bytes && observed.work == normal.work);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits short_limit = exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis == 1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts = 0; CHECK(pipeline(fixture,&short_limit, &observed) == XR_XIR_BUDGET);
    }
    CHECK(!physical);
    puts("Initializer Source full physical FI, three exact/minus-one budgets and no partial publication passed");
}
int main(int argc,char **argv) {
    bool baseline=argc==2 && !strcmp(argv[1],"--comptime-baseline");
    size_t first=0,last=1;
    if (baseline) { first=1;last=3; }
    else if (argc==2 && !strcmp(argv[1],"--comptime-array")) { first=1;last=2; }
    else if (argc==2 && !strcmp(argv[1],"--comptime-optional-tuple")) { first=2;last=3; }
    else if (argc==2 && !strcmp(argv[1],"--field-places")) { first=3;last=4; }
    else if (argc==2 && !strcmp(argv[1],"--field-places-baseline")) { first=3;last=4;baseline=true; }
    else if (argc==2 && !strcmp(argv[1],"--struct-spread-order")) { first=4;last=5; }
    else if (argc==2 && !strcmp(argv[1],"--struct-spread-cow")) { first=5;last=6; }
    else if (argc==2 && !strcmp(argv[1],"--struct-spread-baseline")) { first=4;last=6;baseline=true; }
    else CHECK(argc==1);
    for (size_t i=first;i<last;++i) initializer_case(&cases[i],baseline);
    return 0;
}
