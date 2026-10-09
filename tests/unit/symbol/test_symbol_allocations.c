/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_symbol_allocations.c - Fixed symbol allocation and rollback expectations
 *
 * KEY CONCEPT:
 *   Source-derived allocation plans check rollback without changing production owners.
 */
#include "xr_symbol_allocation_probe.h"
#include "runtime/symbol/xsymbol_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
_Static_assert(SYMBOL_BUILTIN_COUNT == 255, "Review the independent builtin allocation oracle");
typedef union MapOwnerModel {
    XrOsIoPolicy policy;
    long double alignment;
    void *pointer;
    uint64_t integer;
} MapOwnerModel;
typedef struct Allocation { void *memory; size_t bytes, ordinal; } Allocation;
typedef struct Expected { size_t bytes; bool resize; const char *source; } Expected;
static Allocation live[600];
static Expected expected[8];
static size_t expected_count, attempts, fail_at, serial, blocks, bytes_live;
static size_t releases[600], release_count;
static xr_rwlock_t *active_lock;
static size_t lock_inits, lock_destroys;
static char names[258][24];
static const char *saved_names[258];

static const char *basename_of(const char *source) {
    const char *name = source;
    for (; *source; ++source) if (*source == '/' || *source == '\\') name = source + 1;
    return name;
}
static size_t slot_for(void *memory) {
    for (size_t i = 0; i < 600; ++i) if (live[i].memory == memory) return i;
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
XR_FUNC void *xr_symbol_test_malloc(size_t bytes, const char *source) {
    if (!admitted(bytes, false, source)) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory && blocks < 600 && bytes_live <= SIZE_MAX - bytes);
    size_t slot = 0;
    while (slot < 600 && live[slot].memory) ++slot;
    CHECK(slot < 600);
    live[slot] = (Allocation){memory, bytes, serial};
    ++blocks; bytes_live += bytes;
    return memory;
}
XR_FUNC void *xr_symbol_test_realloc(void *memory, size_t bytes, const char *source) {
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
XR_FUNC void xr_symbol_test_free(void *memory) {
    if (!memory) return;
    size_t slot = slot_for(memory);
    CHECK(blocks && release_count < 600 && bytes_live >= live[slot].bytes);
    releases[release_count++] = live[slot].ordinal;
    --blocks; bytes_live -= live[slot].bytes;
    live[slot] = (Allocation){0};
    xr_free(memory);
}
static void phase(const Expected *plan, size_t count, size_t failure) {
    CHECK(count <= 8);
    if (count) memcpy(expected, plan, count * sizeof(*plan));
    expected_count = count; attempts = release_count = 0; fail_at = failure;
}

XR_FUNC void xr_symbol_test_rwlock_init(xr_rwlock_t *lock) {
    CHECK(!active_lock && lock_inits == lock_destroys);
    xr_rwlock_init(lock); active_lock = lock; ++lock_inits;
}
XR_FUNC void xr_symbol_test_rwlock_destroy(xr_rwlock_t *lock) {
    CHECK(active_lock == lock && lock_inits == lock_destroys + 1);
    xr_rwlock_destroy(lock); active_lock = NULL; ++lock_destroys;
}
static void empty(void) {
    CHECK(!blocks && !bytes_live && !active_lock && lock_inits == lock_destroys);
    for (size_t i = 0; i < 600; ++i) CHECK(!live[i].memory);
}
static void reset(void) {
    empty(); serial = lock_inits = lock_destroys = 0;
    for (int i = 0; i < 258; ++i) {
        CHECK(snprintf(names[i], sizeof(names[i]), "User%03d", i) == 7);
        saved_names[i] = NULL;
    }
}
static void create_plan(size_t failure) {
    const Expected plan[] = {
        {sizeof(XrSymbolTable), false, "xsymbol_table.c"},
        {sizeof(MapOwnerModel) + sizeof(XrHashMap), false, "xio_policy.c"},
        {16 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {256 * sizeof(const char *), false, "xsymbol_table.c"}
    };
    phase(plan, 4, failure);
}
static void initial_table(XrSymbolTable *table) {
    CHECK(table && !table->count && !table->builtin_count && table->capacity == 256);
    CHECK(table->name_to_id && !table->name_to_id->count && table->name_to_id->capacity == 16);
    for (int i = 0; i < 256; ++i) CHECK(!table->id_to_name[i]);
    CHECK(blocks == 4 && active_lock == &table->lock && lock_inits == 1 && !lock_destroys);
    CHECK(bytes_live == sizeof(XrSymbolTable) + sizeof(MapOwnerModel) + sizeof(XrHashMap) +
        16 * sizeof(XrHashMapEntry) + 256 * sizeof(const char *));
}
static XrSymbolTable *start(void) {
    reset(); create_plan(0); XrSymbolTable *table = xr_symbol_table_create();
    CHECK(attempts == 4); initial_table(table); return table;
}
static void finish(XrSymbolTable *table) {
    phase(NULL, 0, 0); xr_symbol_table_destroy(table); empty();
}
static void create_cases(void) {
    static const size_t order[4][3] = {{0,0,0},{1,0,0},{2,1,0},{3,2,1}};
    for (size_t failure = 1; failure <= 4; ++failure) {
        reset(); create_plan(failure); CHECK(!xr_symbol_table_create());
        CHECK(attempts == failure && release_count == failure - 1 && !lock_inits); empty();
        for (size_t i = 0; i < release_count; ++i) CHECK(releases[i] == order[failure - 1][i]);
        phase(NULL, 0, 0); xr_symbol_table_destroy(NULL); empty();
        create_plan(0); XrSymbolTable *retry = xr_symbol_table_create();
        CHECK(attempts == 4); initial_table(retry); finish(retry);
    }
    XrSymbolTable *table = start(); finish(table);
    const size_t order_ok[] = {3,2,4,1};
    CHECK(release_count == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(releases[i] == order_ok[i]);
}
static void builtins_plan(size_t failure) {
    const Expected plan[] = {
        {sizeof(MapOwnerModel) + sizeof(XrHashMap), false, "xio_policy.c"},
        {16 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {32 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {64 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {128 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {256 * sizeof(XrHashMapEntry), false, "xio_policy.c"},
        {512 * sizeof(XrHashMapEntry), false, "xio_policy.c"}
    };
    phase(plan, 7, failure);
}
static void builtin_identities(XrSymbolTable *table) {
    static const struct { const char *name; SymbolId id; } pairs[] = {
        {"length", SYMBOL_LENGTH}, {"delete", SYMBOL_DELETE}, {"push", SYMBOL_PUSH},
        {"toString", SYMBOL_TOSTRING}, {"error", SYMBOL_ERROR}, {"size", SYMBOL_SIZE},
        {"checkedAdd", SYMBOL_CHECKED_ADD}, {"copyBytes", SYMBOL_COPY_BYTES}
    };
    CHECK(table->count >= 254 && table->builtin_count == 254 && table->name_to_id->capacity == 512);
    for (size_t i = 0; i < sizeof(pairs)/sizeof(pairs[0]); ++i) {
        CHECK(xr_symbol_lookup_in_table(table, pairs[i].name) == pairs[i].id);
        CHECK(!strcmp(xr_symbol_get_name_in_table(table, pairs[i].id), pairs[i].name));
    }
    for (int i = 1; i <= 254; ++i) {
        const char *name = xr_symbol_get_name_in_table(table, i);
        CHECK(name && xr_symbol_lookup_in_table(table, name) == i);
    }
}
static void builtin_cases(void) {
    static const size_t order[7][6] = {
        {0},{1},{2,1},{2,3,1},{2,3,4,1},{2,3,4,5,1},{2,3,4,5,6,1}
    };
    for (size_t failure = 1; failure <= 7; ++failure) {
        XrSymbolTable *table = start();
        XrHashMap *map = table->name_to_id;
        const char **array = table->id_to_name;
        size_t base = serial;
        builtins_plan(failure); CHECK(!xr_symbol_table_init_builtins(table));
        CHECK(attempts == failure && release_count == failure - 1);
        for (size_t i = 0; i < release_count; ++i) CHECK(releases[i] == base + order[failure - 1][i]);
        CHECK(table->name_to_id == map && table->id_to_name == array); initial_table(table);
        phase(NULL, 0, 0); CHECK(xr_symbol_lookup_in_table(table, "length") == SYMBOL_INVALID);
        builtins_plan(0); CHECK(xr_symbol_table_init_builtins(table) && attempts == 7);
        CHECK(table->count == 254 && blocks == 4 && active_lock == &table->lock);
        CHECK(bytes_live == sizeof(XrSymbolTable) + sizeof(MapOwnerModel) + sizeof(XrHashMap) +
            512 * sizeof(XrHashMapEntry) + 256 * sizeof(const char *));
        phase(NULL, 0, 0); builtin_identities(table);
        map = table->name_to_id; array = table->id_to_name;
        CHECK(xr_symbol_table_init_builtins(table));
        CHECK(table->name_to_id == map && table->id_to_name == array && !attempts);
        const Expected user = {8, false, "xsymbol_table.c"};
        phase(&user, 1, 0); CHECK(xr_symbol_register_in_table(table, "User000") == 255 && attempts == 1);
        const char *user_name = xr_symbol_get_name_in_table(table, 255);
        phase(NULL, 0, 0); CHECK(xr_symbol_table_init_builtins(table));
        CHECK(table->count == 255 && table->id_to_name == array && table->name_to_id == map);
        CHECK(xr_symbol_get_name_in_table(table, 255) == user_name && !strcmp(user_name, "User000"));
        builtin_identities(table); finish(table);
    }
}
static void dynamic_plan(int prior, bool array_already_grown, size_t failure) {
    Expected plan[2]; size_t count = 0;
    if (prior == 256 && !array_already_grown)
        plan[count++] = (Expected){512 * sizeof(const char *), true, "xsymbol_table.c"};
    plan[count++] = (Expected){8, false, "xsymbol_table.c"};
    if (prior == 12 || prior == 24 || prior == 48 || prior == 96 || prior == 192) {
        size_t capacity = prior == 12 ? 32u : prior == 24 ? 64u : prior == 48 ? 128u :
            prior == 96 ? 256u : 512u;
        CHECK(count < 2);
        plan[count++] = (Expected){capacity * sizeof(XrHashMapEntry), false, "xio_policy.c"};
    }
    phase(plan, count, failure);
}
static void dynamic_identities(XrSymbolTable *table, int count) {
    CHECK(table->count == count && !table->builtin_count && table->name_to_id->count == (uint32_t)count);
    for (int i = 0; i < count; ++i) {
        CHECK(table->id_to_name[i] == saved_names[i]);
        CHECK(!strcmp(saved_names[i], names[i]));
        CHECK(xr_symbol_get_name_in_table(table, i + 1) == saved_names[i]);
        CHECK(xr_symbol_lookup_in_table(table, names[i]) == i + 1);
    }
}
static void fill(XrSymbolTable *table, int count) {
    for (int i = 0; i < count; ++i) {
        dynamic_plan(i, false, 0);
        CHECK(xr_symbol_register_in_table(table, names[i]) == i + 1 && attempts == expected_count);
        saved_names[i] = xr_symbol_get_name_in_table(table, i + 1);
    }
    phase(NULL, 0, 0); dynamic_identities(table, count);
}
static void dynamic_failures(int count) {
    for (size_t failure = 1; failure <= 2; ++failure) {
        XrSymbolTable *table = start(); fill(table, count);
        size_t prior_blocks = blocks, prior_bytes = bytes_live;
        const char **old_array = table->id_to_name;
        XrHashMapEntry *old_entries = table->name_to_id->entries;
        uint32_t old_capacity = table->name_to_id->capacity;
        dynamic_plan(count, false, failure);
        CHECK(xr_symbol_register_in_table(table, names[count]) == SYMBOL_INVALID && attempts == failure);
        bool retained_grow = count == 256 && failure == 2;
        CHECK(blocks == prior_blocks && bytes_live == prior_bytes +
            (retained_grow ? 256 * sizeof(const char *) : 0));
        CHECK(release_count == (count == 12 && failure == 2 ? 1u : 0u));
        CHECK(table->capacity == (retained_grow ? 512 : 256));
        if (!retained_grow) CHECK(table->id_to_name == old_array);
        CHECK(table->name_to_id->entries == old_entries && table->name_to_id->capacity == old_capacity);
        if (count < table->capacity) CHECK(!table->id_to_name[count]);
        phase(NULL, 0, 0); dynamic_identities(table, count);
        CHECK(xr_symbol_lookup_in_table(table, names[count]) == SYMBOL_INVALID);
        dynamic_plan(count, retained_grow, 0);
        CHECK(xr_symbol_register_in_table(table, names[count]) == count + 1 && attempts == expected_count);
        saved_names[count] = xr_symbol_get_name_in_table(table, count + 1);
        phase(NULL, 0, 0); dynamic_identities(table, count + 1);
        CHECK(xr_symbol_register_in_table(table, names[0]) == 1 && !attempts);
        CHECK(!xr_symbol_get_name_in_table(table, INT32_MIN));
        finish(table);
    }
}
static void user_first(void) {
    XrSymbolTable *table = start(); fill(table, 1);
    XrHashMap *map = table->name_to_id; const char **array = table->id_to_name;
    CHECK(!xr_symbol_table_init_builtins(table) && !attempts);
    CHECK(table->name_to_id == map && table->id_to_name == array); dynamic_identities(table, 1);
    CHECK(xr_symbol_lookup_in_table(table, "length") == SYMBOL_INVALID);
    finish(table);
}
int main(void) {
    create_cases(); builtin_cases(); dynamic_failures(12); dynamic_failures(256); user_first(); empty();
    puts("symbol allocations: fixed create/staged/dynamic failures and physical ownership passed");
    return 0;
}
