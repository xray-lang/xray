/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtoml_allocations.c - Observe actual TOML policy allocations and work
 */
#include "base/xtoml.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, allocated_total, peak, live_count;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live_count; live += bytes; allocated_total += bytes;
    if (live > peak) peak = live;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096);
    live -= allocations[i].bytes; --live_count;
    allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats s;
    CHECK(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes == live && s.allocated_bytes == allocated_total && s.peak_bytes == peak);
    return s;
}
static void reset(size_t failure) {
    CHECK(!live && !live_count);
    attempts = allocated_total = peak = 0; fail_at = failure;
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};

#include "../../../src/base/xtoml.c"
#define REQUIRE CHECK
static unsigned char canary;
static XrCompileResourceStats measured;
static size_t run_limits(const char *source, size_t failure, const XrCompileResourceLimits *limits,
    XrTomlParseLimits shape, XrTomlParseStatus expected) {
    reset(failure ? failure-1 : SIZE_MAX);
    XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits,&owner);
    if (created != XR_COMPILE_RESOURCE_OK) {
        CHECK(expected == (created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_TOML_PARSE_OUT_OF_MEMORY : XR_TOML_PARSE_BUDGET));
        CHECK(!owner && !live); measured = (XrCompileResourceStats){0}; return attempts;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(owner);
    XrTomlValue *output = (void *)&canary;
    XrTomlParseStatus status = xtoml_parse_owned(&policy,source,strlen(source),&shape,&output);
    if (status != expected) fprintf(stderr,"failure=%zu work=%llu expected=%d got=%d source=%.80s\n",
        failure,(unsigned long long)limits->work,expected,status,source);
    CHECK(status == expected);
    if (status != XR_TOML_PARSE_OK) CHECK(output == (void *)&canary);
    else { CHECK(output != (void *)&canary); xtoml_owned_free(output); }
    CHECK(live_count == 1);
    measured = stats(owner);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
    return attempts;
}
static size_t run(const char *source, size_t failure, bool valid) {
    size_t total = run_limits(source,failure,&unlimited,(XrTomlParseLimits){65536,128},
        failure ? XR_TOML_PARSE_OUT_OF_MEMORY : valid ? XR_TOML_PARSE_OK : XR_TOML_PARSE_INVALID);
    if (failure) CHECK(attempts == failure);
    return total;
}
static void budget_boundaries(void) {
    const char *source = "a.b=[1,2,3]\nx=\"owned\\u0000suffix\"\n";
    size_t sites = run(source,0,true); XrCompileResourceStats baseline = measured;
    for (size_t failure = 1; failure <= sites; ++failure) run(source,failure,true);
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned below = 0; below < 2; ++below) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes-below;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes-below;
        else limits.work = baseline.work-below;
        run_limits(source,0,&limits,(XrTomlParseLimits){65536,128},below ? XR_TOML_PARSE_BUDGET : XR_TOML_PARSE_OK);
    }
    run_limits(source,0,&unlimited,(XrTomlParseLimits){strlen(source)-1,128},XR_TOML_PARSE_LIMIT);
    CHECK(attempts == 1);
    printf("TOML physical allocation sites=%zu allocated=%llu peak=%llu; all three exact/minus1 PASS\n",
        sites,(unsigned long long)baseline.allocated_bytes,(unsigned long long)baseline.peak_bytes);
}
static void require_depth(const char *source, uint32_t depth, bool valid) {
    run_limits(source,0,&unlimited,(XrTomlParseLimits){65536,depth},valid ? XR_TOML_PARSE_OK : XR_TOML_PARSE_LIMIT);
}
static void depth_boundaries(void) {
    require_depth("",0,true); require_depth("x=1\n",0,false); require_depth("x=1\n",1,true);
    const char *sources[] = {"a=[[[0]]]\n","[a.b]\nc.d=1\n","a.b={c=[{d=1}]}\n","[[a.b]]\nc=[1]\n"};
    const uint32_t depths[] = {4,4,5,5};
    for (unsigned i = 0; i < 4; ++i) { require_depth(sources[i],depths[i]-1,false); require_depth(sources[i],depths[i],true); }
    char deep[20008];
    for (unsigned nesting = 127; nesting <= 128; ++nesting) {
        size_t pos = 0; deep[pos++] = 'a'; deep[pos++] = '=';
        for (unsigned i = 0; i < nesting; ++i) deep[pos++] = '[';
        deep[pos++] = '0'; for (unsigned i = 0; i < nesting; ++i) deep[pos++] = ']'; deep[pos] = 0;
        require_depth(deep,UINT32_MAX,nesting == 127);
    }
    memcpy(deep,"a=",2); memset(deep+2,'[',5000); deep[5002] = '0';
    memset(deep+5003,']',5000); deep[10003] = 0; require_depth(deep,128,false);
    for (unsigned i = 0; i < 5000; ++i) { deep[i*2] = 'a'; deep[i*2+1] = '.'; }
    memcpy(deep+9999,"=1",3); require_depth(deep,128,false);
    memcpy(deep,"a=",2); for (unsigned i = 0; i < 5000; ++i) memcpy(deep+2+i*3,"{a=",3);
    deep[15002] = '0'; memset(deep+15003,'}',5000); deep[20003] = 0; require_depth(deep,128,false);
}
static void work_boundaries(void) {
    const char *source = "same_prefix_aaaa=1\nsame_prefix_aaab=2\nsame_prefix_aaac=3\n";
    run(source,0,true); XrCompileResourceStats baseline = measured;
    for (uint64_t work = 1; work < baseline.work; ++work) {
        XrCompileResourceLimits limits = unlimited; limits.work = work;
        run_limits(source,0,&limits,(XrTomlParseLimits){65536,128},XR_TOML_PARSE_BUDGET);
    }
    printf("TOML all work limits 1..%llu rejected with physical zero\n",(unsigned long long)baseline.work-1);
    const char *numbers = "a=[0.1,1e-308,1e309,2.2250738585072014e-308,9007199254740993.0,0x7fff_ffff_ffff_ffff]\n";
    run(numbers,0,true); baseline = measured;
    for (unsigned i = 1; i <= 128; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = (baseline.work*i)/129;
        run_limits(numbers,0,&limits,(XrTomlParseLimits){65536,128},XR_TOML_PARSE_BUDGET);
    }
    XrCompileResourceLimits exact = unlimited; exact.work = baseline.work;
    run_limits(numbers,0,&exact,(XrTomlParseLimits){65536,128},XR_TOML_PARSE_OK);
    --exact.work; run_limits(numbers,0,&exact,(XrTomlParseLimits){65536,128},XR_TOML_PARSE_BUDGET);
}
static void ownership_and_query(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(owner); const XrTomlParseLimits limits = {1024,128};
    char source[] = "same_prefix_aaab=42\n";
    XrTomlValue *root = NULL, *value = (void *)&canary;
    CHECK(xtoml_parse_owned(&policy,source,strlen(source),&limits,&root) == XR_TOML_PARSE_OK);
    XrCompileResourceStats before = stats(owner);
    CHECK(xtoml_owned_get(root,"same_prefix_aaab",&value) == XR_TOML_PARSE_OK && value->as.integer == 42);
    XrCompileResourceStats after = stats(owner);
    CHECK(after.work-before.work == 17+1+16);
    memset(source,0,sizeof(source)); xr_compile_resources_release(owner);
    CHECK(xtoml_owned_get(root,"absent",&value) == XR_TOML_PARSE_OK && !value);
    CHECK(xtoml_owned_get(root,"same_prefix_aaab",&value) == XR_TOML_PARSE_OK && value->as.integer == 42);
    xtoml_owned_free(root); CHECK(!live && !live_count);
    run("x=1\n",0,true); XrCompileResourceLimits capped = unlimited; capped.work = measured.work;
    reset(SIZE_MAX); owner = NULL; root = NULL;
    CHECK(xr_compile_resources_new(&capped,&owner) == XR_COMPILE_RESOURCE_OK); policy = xr_compile_io_policy(owner);
    CHECK(xtoml_parse_owned(&policy,"x=1\n",4,&limits,&root) == XR_TOML_PARSE_OK);
    value = (void *)&canary;
    CHECK(xtoml_owned_get(root,"x",&value) == XR_TOML_PARSE_BUDGET && value == (void *)&canary);
    XrTomlValue *other = (void *)&canary;
    CHECK(xtoml_parse_owned(&policy,"x=1\n",4,&limits,&other) == XR_TOML_PARSE_BUDGET && other == (void *)&canary);
    xr_compile_resources_release(owner); xtoml_owned_free(root); CHECK(!live && !live_count);
}
typedef struct RejectPolicy {
    XrCompileResources *owner;
    XrOsIoStatus failure;
    size_t calls, fail_at;
} RejectPolicy;
static XrOsIoStatus reject_alloc(void *context, size_t bytes, void **output) {
    RejectPolicy *state = context; XrOsIoPolicy policy = xr_compile_io_policy(state->owner);
    return policy.alloc(policy.context,bytes,output);
}
static void reject_free(void *context, void *memory) { (void)context; xr_compile_resources_free(memory); }
static XrOsIoStatus reject_work(void *context, uint64_t units) {
    RejectPolicy *state = context;
    if (++state->calls == state->fail_at) return state->failure;
    XrOsIoPolicy policy = xr_compile_io_policy(state->owner); return policy.work(policy.context,units);
}
static void policy_failures(void) {
    const char *source = "x={a=[1,2,3],b=\"long\\u0041 value\"}\n";
    const XrOsIoStatus errors[] = {XR_OS_IO_BUDGET,XR_OS_IO_OUT_OF_MEMORY,XR_OS_IO_IO,XR_OS_IO_BAD_ARGUMENT};
    const XrTomlParseStatus expected[] = {XR_TOML_PARSE_BUDGET,XR_TOML_PARSE_OUT_OF_MEMORY,XR_TOML_PARSE_IO,XR_TOML_PARSE_BAD_ARGUMENT};
    for (unsigned kind = 0; kind < 4; ++kind) for (size_t point = 1; point < 256; ++point) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        RejectPolicy state = {owner,errors[kind],0,point};
        XrOsIoPolicy policy = {&state,reject_alloc,reject_free,reject_work};
        const XrTomlParseLimits limits = {1024,128}; XrTomlValue *root = (void *)&canary;
        XrTomlParseStatus status = xtoml_parse_owned(&policy,source,strlen(source),&limits,&root);
        CHECK(status == expected[kind] && root == (void *)&canary && state.calls == point);
        CHECK(live_count == 1); xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(owner);
    const XrTomlParseLimits limits = {1024,128}; XrTomlValue *root = (void *)&canary;
    CHECK(xtoml_parse_owned(NULL,"",0,&limits,&root) == XR_TOML_PARSE_BAD_ARGUMENT && root == (void *)&canary);
    CHECK(xtoml_parse_owned(&policy,"",0,NULL,&root) == XR_TOML_PARSE_BAD_ARGUMENT && root == (void *)&canary);
    TomlCtx context = {0}; context.policy = policy;
    CHECK(!toml_allocate(&context,SIZE_MAX,false) && context.status == XR_TOML_PARSE_BUDGET && attempts == 1);
    context = (TomlCtx){0}; context.policy = policy;
    CHECK(!grown_capacity(&context,INT_MAX,sizeof(XrTomlMember)) && context.status == XR_TOML_PARSE_LIMIT);
    context = (TomlCtx){0}; context.policy = policy;
    char *memory = toml_allocate(&context,4,false); CHECK(memory); memcpy(memory,"abc",4);
    size_t prior_live = live; fail_at = attempts;
    CHECK(!toml_resize(&context,memory,8) && context.status == XR_TOML_PARSE_OUT_OF_MEMORY);
    CHECK(live == prior_live && !memcmp(memory,"abc",4)); fail_at = SIZE_MAX;
    toml_release(memory); xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static void utf8_read_work(void) {
    /* The shared decoder reads E0/80, then diagnostics read E0/80/BF. */
    const char invalid[] = "\xe0\x80\xbf";
    run(invalid,0,false); CHECK(measured.work == 6 && attempts == 1);
    for (uint64_t work = 1; work < 6; ++work) {
        XrCompileResourceLimits limits = unlimited; limits.work = work;
        run_limits(invalid,0,&limits,(XrTomlParseLimits){1024,128},XR_TOML_PARSE_BUDGET);
        CHECK(attempts == 1);
    }
}
int main(void) {
    policy_failures(); utf8_read_work();
    ownership_and_query();
    depth_boundaries();
    work_boundaries();
    budget_boundaries();
    static const char source[] =
        "title=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"\n"
        "a.b.c.d.e.f=1\na.b.c.d.e.g=[1,2,3,4,5,6,7,8,9]\n"
        "empty=\"\"\nempty_literal=''\n\"quoted\"='literal'\n"
        "continued=\"\"\"first\\ \t\r\n \t\n second\"\"\"\n"
        "one=\"\"\"x\"\"\"\"\n"
        "two='''x'''''\n"
        "numbers=[-9223372036854775808,9223372036854775807,0x7fff_ffff_ffff_ffff,0o755,0b1010,1_000.2_5e+0_2,-0.0]\n"
        "dates=[2024-02-29,07:32:00.123456789,1979-05-27 07:32:00.123456789+01:30]\n"
        "escaped=\"\\b\\t\\n\\f\\r\\\"\\\\\\u0041\\U0001F600\"\n"
        "\"a\\u0000b\"=\"x\\u0000y\"\n\"a\\u0000c\"='distinct'\n"
        "\"nested\\u0000key\".child=\"owned\\u0000suffix\"\n"
        "map={a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9}\n"
        "[settings.parent]\nx=1\n"
        "[[items]]\nx=1\n[[items]]\nx=2\n[[items]]\nx=3\n"
        "[[items]]\nx=4\n[[items]]\nx=5\n[[items]]\nx=6\n"
        "[[items]]\nx=7\n[[items]]\nx=8\n[[items]]\nx=9\n";
    size_t total = run(source, 0, true);
    for (size_t failure = 1; failure <= total; ++failure)
        run(source, failure, true);
    static const char *const invalid[] = {
        "x={a=1,a=2}\n", "x=1\nx.a=2\n",
        "\"a\\u0000b\"=1\n\"a\\u0000b\"=2\n",
        "\"a\\u0000b\"=1\n\"a\\u0000b\".child=2\n",
        "[a]\nx=1\n[a]\ny=2\n",
        "a.b=1\n[a]\nc=2\n",
        "a={b=1}\na.c=2\n",
        "a={}\n[a.b]\nx=1\n",
        "a=[]\n[[a]]\nx=1\n",
        "a=[{b=1}]\n[a.c]\nx=1\n",
        "[[a]]\nx=1\n[a]\ny=2\n",
        "[a.b]\nx=1\n[a]\nb.y=2\n",
        "a={b={c=1},b.d=2}\n",
        "a={b=1}\n[[a.c]]\nx=1\n",
        "version=1 junk\n",
        "flag=true false\n",
        "[declarations] junk\nversion=1\n",
        "[[items]] junk\nx=1\n",
        "name=\"bad\\q\"\n",
        "name=\"bad\\\ncontinued\"\n",
        "name=\"\"\"bad\\  text\"\"\"\n",
        "\"\"\"key\"\"\"=1\n",
        "'''key'''=1\n",
        "name=\"\\u1",
        "name=\"\\U00110000\"\n",
        "name=\"\\uD800\"\n",
        "version=1+2\n", "version=01\n", "version=1_\n",
        "a=0x_FF\n", "a=+0x1\n", "a=1.e2\n", "a=1e_2\n",
        "a=9223372036854775808\n", "a=-9223372036854775809\n", "a=0x8000000000000000\n",
        "a=2023-02-29\n", "a=2024-01-01T24:00:00Z\n", "a=2024-01-01T00:00:00+00:60\n",
        "a=2024-01-01T00:00:00.\n", "a=07:32:00Z\n"
    };
    for (size_t i = 0; i < XR_COUNTOF(invalid); ++i) {
        size_t count = run(invalid[i], 0, false);
        for (size_t failure = 1; failure <= count; ++failure)
            run(invalid[i], failure, false);
        total += count;
    }
    printf("TOML allocation checks: %zu failures injected, zero live allocations/bytes\n", total);
    return 0;
}
