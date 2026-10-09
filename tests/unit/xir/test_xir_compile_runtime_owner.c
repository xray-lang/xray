/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_runtime_owner.c - Compile metadata outlives producers and domains
 */
#include "base/xmalloc.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include <stdio.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, physical, peak, total;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live; physical += bytes; total += bytes;
    if (physical > peak) peak = physical;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096 && live);
    size_t bytes = allocations[i].bytes;
    xr_free(memory);
    allocations[i] = (Allocation){0}; --live; physical -= bytes;
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static void reset_observer(void) {
    CHECK(!live && !physical);
    attempts = peak = total = 0; fail_at = SIZE_MAX;
}
static XrXirCompileContext context_new(uint64_t work) {
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, work};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats value;
    CHECK(xr_compile_resources_stats(context->resources, &value) == XR_COMPILE_RESOURCE_OK);
    CHECK(value.live_bytes == physical && value.peak_bytes == peak && value.allocated_bytes == total);
    return value;
}

static XrXirValueStatus arena_fixture(const XrXirCompileContext *context, XrXirTypeArena **out) {
    XrXirNominalVariant variants[] = {{{"None",4},0,0},{{"Some",4},0,1}};
    XrXirNominalFieldIdentity inner_fields[] = {{{"text",4},0}};
    XrXirNominalFieldIdentity outer_fields[] = {{{"choice",6},0}};
    XrXirNominalIdentity identities[] = {
        {{"alpha",5},{"Choice",6},1,0,inner_fields,1,XR_XIR_NOMINAL_ENUM,variants,2,0,{0}},
        {{"alpha",5},{"Outer",5},1,0,outer_fields,1,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}}};
    XrXirNominalTable table = {NULL,2,identities};
    XrXirType inner_types[] = {XR_XIR_STRING}, outer_types[] = {(XrXirType)256};
    XrXirTypeNode nodes[2] = {{0}};
    nodes[0].kind = nodes[1].kind = XR_XIR_TYPE_NOMINAL;
    nodes[0].nominal = (XrXirNominalType){0,NULL,0,inner_types,1};
    nodes[1].nominal = (XrXirNominalType){1,NULL,0,outer_types,1};
    XrXirTypes types = {nodes,2,&table,NULL};
    return xr_xir_compile_type_arena_new(context, &types, out);
}
static void arena_lifetime_and_authenticity(void) {
    reset_observer();
    XrXirCompileContext context = context_new(UINT64_MAX);
    XrXirTypeArena *arena = NULL;
    CHECK(arena_fixture(&context,&arena) == XR_XIR_VALUE_OK);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    CHECK(types && types->count == 2 && !memcmp(types->nominals->identities[1].name.bytes,"Outer",5));
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX,&domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats before = xr_xir_domain_stats(domain);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,UINT64_MAX,UINT64_MAX};
    XrXirValue empty={0},copy={0},outer={0};
    CHECK(xr_xir_enum_new((XrXirType)256,0,NULL,0,&admission,&empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&empty,&copy) == XR_XIR_VALUE_OK);
    const XirNominalValue *canonical = (const XirNominalValue *)(intptr_t)empty.payload;
    CHECK(!canonical->object.domain && !canonical->count && !canonical->fields);
    XrXirDomainStats after = xr_xir_domain_stats(domain);
    CHECK(before.live_bytes == after.live_bytes && before.allocations == after.allocations);
    CHECK(xr_xir_struct_new((XrXirType)257,&empty,1,&admission,&outer) == XR_XIR_VALUE_OK);
    XirNominalValue forged = *canonical;
    atomic_init(&forged.object.references,1);
    XrXirValue fake = {(uint32_t)256,0,(int64_t)(intptr_t)&forged};
    CHECK(!xr_xir_value_valid(&fake));
    XirNominalValue *record = (XirNominalValue *)(intptr_t)outer.payload;
    XrXirValue saved = record->fields[0];
    record->fields[0] = fake;
    CHECK(!xr_xir_value_valid(&outer));
    record->fields[0] = saved;
    CHECK(xr_xir_value_valid(&outer));
    forged.object.kind = XR_XIR_TYPE_ARRAY;
    CHECK(!xr_xir_value_valid(&fake));
    forged = *canonical; forged.variant = UINT32_MAX;
    CHECK(!xr_xir_value_valid(&fake));
    XrCompileResourceStats active = stats(&context);
    xr_xir_compile_type_arena_drop(arena);
    xr_compile_resources_release(context.resources);
    xr_xir_domain_drop(domain);
    CHECK(xr_xir_value_valid(&outer) && xr_xir_value_valid(&copy));
    CHECK(stats(&context).work == active.work);
    xr_xir_value_drop(&outer);
    xr_xir_value_drop(&empty);
    XrXirValue survivor={0};
    CHECK(xr_xir_value_copy(&copy,&survivor) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&copy);
    CHECK(xr_xir_value_valid(&survivor));
    uint32_t variant=99;
    CHECK(xr_xir_enum_variant(&survivor,&variant) == XR_XIR_VALUE_OK && variant == 0);
    xr_xir_value_drop(&survivor);
    CHECK(!live && !physical);
}
static void arena_allocation_failures(void) {
    size_t count=0;
    for (size_t failure=SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext context=context_new(UINT64_MAX);
        fail_at=failure;
        XrXirTypeArena *arena=NULL;
        XrXirValueStatus status=arena_fixture(&context,&arena);
        if (failure==SIZE_MAX) { CHECK(status==XR_XIR_VALUE_OK); count=attempts; }
        else { CHECK(status==XR_XIR_VALUE_OOM && !arena); }
        xr_xir_compile_type_arena_drop(arena);
        CHECK(stats(&context).allocation_count >= 1);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
        if (failure==SIZE_MAX) failure=1;
        else if (++failure==count) break;
    }
    printf("Type arena actual allocation OOM points: %zu\n",count-1);
}
static void arena_budget_and_output(void) {
    reset_observer();
    XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirTypeArena *untouched=(XrXirTypeArena *)(uintptr_t)1;
    CHECK(arena_fixture(&context,&untouched) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(untouched == (XrXirTypeArena *)(uintptr_t)1 && attempts==1);
    xr_compile_resources_release(context.resources);
    for (uint64_t work=1;work<128;++work) {
        reset_observer();context=context_new(work);
        XrXirTypeArena *arena=NULL;
        CHECK(arena_fixture(&context,&arena) == XR_XIR_VALUE_LIMIT && !arena);
        CHECK(stats(&context).work <= work);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
}
#include "xir_compile_program_owner_cases.h"
#include "xir_construction_owner_cases.h"
int main(void) {
    arena_lifetime_and_authenticity(); arena_allocation_failures(); arena_budget_and_output();
    program_lifetime(); program_take_failures(); program_abi_and_owner();
    program_method_kind_match(); construction_cross_owner();
    puts("Canonical metadata: producer/domain release, nested forged handles rejected, physical zero");
    return 0;
}
