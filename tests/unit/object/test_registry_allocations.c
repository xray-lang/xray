/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_registry_allocations.c - Runtime metadata allocation qualification
 *
 * KEY CONCEPT:
 *   Real registry and map allocation sites retain fixed failure and ownership expectations.
 */
#include "xr_registry_allocation_probe.h"
#include "runtime/class/xtype_registry.h"
#include "runtime/xisolate_internal.h"
#include "runtime/xisolate_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef union MapOwnerModel {
    XrOsIoPolicy policy;
    long double alignment;
    void *pointer;
    uint64_t integer;
} MapOwnerModel;
typedef struct Allocation { void *memory; size_t bytes, ordinal; } Allocation;
typedef struct Expected { size_t bytes; bool resize; const char *source; } Expected;
static Allocation live[160];
static Expected expected[4];
static size_t expected_count, attempts, fail_at, serial, blocks, bytes_live;
static size_t releases[160], release_count;
static XrVMRuntime runtime;
static XrRuntimeCore core;
static XrClass classes[70];
static char names[70][24];
static XrTypeMetadata *metadata[70];

static const char *basename_of(const char *source) {
    const char *name = source;
    for (; *source; ++source) if (*source == '/' || *source == '\\') name = source + 1;
    return name;
}
static size_t slot_for(void *memory) {
    for (size_t i = 0; i < 160; ++i) if (live[i].memory == memory) return i;
    fprintf(stderr, "untracked allocation\n"); exit(1);
}
static bool admitted(size_t bytes, bool resize, const char *source) {
    CHECK(attempts < expected_count);
    Expected e = expected[attempts++];
    ++serial;
    CHECK(bytes == e.bytes && resize == e.resize);
    CHECK(!strcmp(basename_of(source), e.source));
    return attempts != fail_at;
}
XR_FUNC void *xr_registry_test_malloc(size_t bytes, const char *source) {
    if (!admitted(bytes, false, source)) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory && blocks < 160 && bytes_live <= SIZE_MAX - bytes);
    size_t slot = 0;
    while (slot < 160 && live[slot].memory) ++slot;
    CHECK(slot < 160);
    live[slot] = (Allocation){memory, bytes, serial};
    ++blocks; bytes_live += bytes;
    return memory;
}
XR_FUNC void *xr_registry_test_realloc(void *memory, size_t bytes, const char *source) {
    size_t slot = slot_for(memory);
    if (!admitted(bytes, true, source)) return NULL;
    void *replacement = xr_realloc(memory, bytes);
    CHECK(replacement != NULL);
    bytes_live -= live[slot].bytes;
    CHECK(bytes_live <= SIZE_MAX - bytes);
    bytes_live += bytes;
    live[slot] = (Allocation){replacement, bytes, serial};
    return replacement;
}
XR_FUNC void xr_registry_test_free(void *memory) {
    if (!memory) return;
    size_t slot = slot_for(memory);
    CHECK(blocks && release_count < 160 && bytes_live >= live[slot].bytes);
    releases[release_count++] = live[slot].ordinal;
    --blocks; bytes_live -= live[slot].bytes;
    live[slot] = (Allocation){0};
    xr_free(memory);
}
static void phase(const Expected *plan, size_t count, size_t failure) {
    CHECK(count <= 4);
    if (count) memcpy(expected, plan, count * sizeof(*plan));
    expected_count = count; attempts = release_count = 0; fail_at = failure;
}
static void empty(void) {
    CHECK(!blocks && !bytes_live && !core.type_registry);
    for (size_t i = 0; i < 160; ++i) CHECK(!live[i].memory);
}
static void reset(void) {
    empty(); serial = 0;
    memset(&runtime, 0, sizeof(runtime)); memset(&core, 0, sizeof(core));
    runtime.core_rt = &core;
    memset(classes, 0, sizeof(classes)); memset(metadata, 0, sizeof(metadata));
    for (int i = 0; i < 70; ++i) {
        CHECK(snprintf(names[i], sizeof(names[i]), "RegistryType%d", i) > 0);
        classes[i].name = names[i];
    }
    classes[0].name = "i64"; classes[1].name = "bool";
}
static void init_plan(size_t failure) {
    const Expected plan[] = {
        {sizeof(XrTypeRegistry), false, "xtype_registry.c"},
        {64 * sizeof(XrTypeMetadata *), false, "xtype_registry.c"},
        {sizeof(MapOwnerModel) + sizeof(XrHashMap), false, "xio_policy.c"},
        {16 * sizeof(XrHashMapEntry), false, "xio_policy.c"}
    };
    phase(plan, 4, failure);
}
static void initialized(void) {
    XrTypeRegistry *r = core.type_registry;
    CHECK(r && r->is_initialized && r->capacity == 64 && !r->type_count);
    CHECK(r->type_map && r->type_map->capacity == 16 && !r->type_map->count);
    CHECK(!r->int_type && !r->float_type && !r->bool_type && !r->string_type);
    CHECK(!r->array_type && !r->map_type && !r->object_type && !r->null_type);
    for (int i = 0; i < 64; ++i) CHECK(!r->types[i]);
    CHECK(blocks == 4 && bytes_live == sizeof(XrTypeRegistry) + 64 * sizeof(XrTypeMetadata *) +
        sizeof(MapOwnerModel) + sizeof(XrHashMap) + 16 * sizeof(XrHashMapEntry));
}
static void start(void) {
    reset(); init_plan(0); xr_registry_init(&runtime); CHECK(attempts == 4); initialized();
}
static void finish(void) {
    phase(NULL, 0, 0); xr_registry_free(&runtime); empty();
    size_t released = release_count; xr_registry_free(&runtime); CHECK(release_count == released);
}
static void init_cases(void) {
    static const size_t order[4][3] = {{0,0,0},{1,0,0},{2,1,0},{3,2,1}};
    for (size_t failure = 1; failure <= 4; ++failure) {
        reset(); init_plan(failure); xr_registry_init(&runtime);
        CHECK(attempts == failure && release_count == failure - 1); empty();
        for (size_t i = 0; i < release_count; ++i) CHECK(releases[i] == order[failure - 1][i]);
        xr_registry_free(&runtime); empty();
        init_plan(0); xr_registry_init(&runtime); CHECK(attempts == 4); initialized();
        XrTypeRegistry *saved = core.type_registry;
        phase(NULL, 0, 0); xr_registry_init(&runtime); CHECK(core.type_registry == saved);
        finish();
    }
    start(); finish();
    const size_t expected_release[] = {4,3,2,1};
    CHECK(release_count == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(releases[i] == expected_release[i]);
}
static void register_plan(int prior, size_t failure) {
    Expected plan[2] = {{sizeof(XrTypeMetadata), false, "xtype_registry.c"}, {0}};
    size_t count = 1;
    if (prior == 12 || prior == 24 || prior == 48) {
        size_t capacity = prior == 12 ? 32u : prior == 24 ? 64u : 128u;
        plan[count++] = (Expected){capacity * sizeof(XrHashMapEntry), false, "xio_policy.c"};
    } else if (prior == 64) {
        plan[count++] = (Expected){128 * sizeof(XrTypeMetadata *), true, "xtype_registry.c"};
    }
    phase(plan, count, failure);
}
static void unchanged(int count) {
    XrTypeRegistry *r = core.type_registry;
    CHECK(r && r->type_count == count && r->type_map->count == (uint32_t)count);
    for (int i = 0; i < count; ++i) {
        CHECK(r->types[i] == metadata[i] && metadata[i]->klass == &classes[i]);
        CHECK(xr_registry_find_type(&runtime, classes[i].name) == metadata[i]);
        CHECK(xr_registry_find_type_by_class(&runtime, &classes[i]) == metadata[i]);
    }
    CHECK(r->int_type == metadata[0] && r->bool_type == (count > 1 ? metadata[1] : NULL));
    CHECK(!r->float_type && !r->string_type && !r->array_type && !r->map_type &&
        !r->object_type && !r->null_type);
}
static void fill(int count) {
    for (int i = 0; i < count; ++i) {
        register_plan(i, 0); metadata[i] = xr_registry_register_class(&runtime, &classes[i]);
        CHECK(metadata[i] && attempts == expected_count);
    }
    phase(NULL, 0, 0); unchanged(count);
}
static void growth_cases(int count) {
    for (size_t failure = 1; failure <= 2; ++failure) {
        start(); fill(count);
        XrTypeRegistry *r = core.type_registry;
        XrTypeMetadata **saved_types = r->types;
        XrHashMapEntry *saved_entries = r->type_map->entries;
        int saved_capacity = r->capacity;
        uint32_t map_capacity = r->type_map->capacity;
        size_t prior_blocks = blocks, prior_bytes = bytes_live;
        register_plan(count, failure);
        CHECK(!xr_registry_register_class(&runtime, &classes[count]));
        CHECK(attempts == failure && release_count == failure - 1);
        CHECK(blocks == prior_blocks && bytes_live == prior_bytes);
        CHECK(r->types == saved_types && r->capacity == saved_capacity);
        CHECK(r->type_map->entries == saved_entries && r->type_map->capacity == map_capacity);
        phase(NULL, 0, 0); unchanged(count);
        CHECK(!xr_registry_find_type(&runtime, classes[count].name));
        register_plan(count, 0); metadata[count] = xr_registry_register_class(&runtime, &classes[count]);
        CHECK(metadata[count] && attempts == 2);
        phase(NULL, 0, 0); unchanged(count + 1);
        CHECK(r->capacity == (count == 64 ? 128 : 64));
        CHECK(r->type_map->capacity == (count == 64 ? 128u : 32u));
        finish();
    }
}
static void snapshots_and_caches(void) {
    start();
    int count = 73; phase(NULL, 0, 0);
    CHECK(!xr_registry_get_all_types(&runtime, &count) && count == 0);
    fill(65); size_t prior_blocks = blocks, prior_bytes = bytes_live;
    const Expected plan = {65 * sizeof(XrTypeMetadata *), false, "xtype_registry.c"};
    phase(&plan, 1, 1); count = 73;
    CHECK(!xr_registry_get_all_types(&runtime, &count) && count == 73 && attempts == 1);
    CHECK(blocks == prior_blocks && bytes_live == prior_bytes && !release_count);
    phase(NULL, 0, 0); unchanged(65);
    phase(&plan, 1, 0); XrTypeMetadata **snapshot = xr_registry_get_all_types(&runtime, &count);
    CHECK(snapshot && count == 65 && attempts == 1);
    for (int i = 0; i < count; ++i) CHECK(snapshot[i] == metadata[i]);
    xr_registry_test_free(snapshot);
    CHECK(blocks == prior_blocks && bytes_live == prior_bytes);
    phase(NULL, 0, 0);
    CHECK(xr_registry_register_class(&runtime, &classes[0]) == metadata[0]);
    XrTypeMetadata duplicate = {&classes[0], NULL};
    CHECK(!xr_registry_register_type(&runtime, &duplicate)); unchanged(65);
    CHECK(!xr_registry_unregister_type(&runtime, "absent")); unchanged(65);
    CHECK(xr_registry_unregister_type(&runtime, "i64"));
    CHECK(!xr_registry_get_int_type(&runtime) && xr_registry_get_bool_type(&runtime) == metadata[1]);
    CHECK(core.type_registry->type_count == 64 && core.type_registry->types[0] == metadata[64]);
    CHECK(!core.type_registry->types[64] && !xr_registry_find_type(&runtime, "i64"));
    for (int i = 1; i < 65; ++i)
        CHECK(xr_registry_find_type(&runtime, classes[i].name) == metadata[i]);
    finish();
}
int main(void) {
    init_cases(); growth_cases(12); growth_cases(64); snapshots_and_caches(); empty();
    puts("registry allocations: fixed init/grow/snapshot rollback and ownership passed");
    return 0;
}
