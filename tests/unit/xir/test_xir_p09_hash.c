/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash.c - Dense key containers, COW and failed-root preservation
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static XrXirTypeArena *hash_test_arena(void) {
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_F64},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_STRING, .result = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_STRING, .result = (XrXirType)259},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = XR_XIR_UNIT},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_STRING}
    };
    XrXirTypes types = {nodes, 7, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 100), &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValue integer(int64_t value) { return (XrXirValue){XR_XIR_I64, 0, value}; }
static XrXirValue hash_test_key(const XirHash *map, size_t index) {
    XrXirLayout layout = {0};
    CHECK(xr_xir_compile_type_arena_layout(map->object.arena, map->key_type, &layout));
    XrXirValue value={0};
    CHECK(storage_leaf_value(map->object.arena,map->key_type,
        hash_span(map,index,false).bytes,layout.size,&value));
    return value;
}
static void hash_expect(const XrXirValue *map, int64_t key, int64_t expected,
    bool exists, XrXirValueAdmission *admission) {
    XrXirValue input = integer(key), result = {0}; bool found = !exists;
    CHECK(xr_xir_map_get(map, &input, admission, &result, &found) == XR_XIR_VALUE_OK && found == exists);
    CHECK(exists ? result.type == XR_XIR_I64 && result.payload == expected : !result.type && !result.payload);
    xr_xir_value_drop(&result);
}
static void hash_dense_cases(XrXirValueAdmission *admission) {
    XrXirValue map = {0}, snapshot = {0};
    CHECK(xr_xir_map_new((XrXirType)256, NULL, NULL, 0, admission, &map) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)256, &map.payload};
    for (int64_t i = 0; i < 512; ++i) {
        XrXirValue key = integer(i), value = integer(i * 7);
        CHECK(xr_xir_map_set(&place, &key, &value, admission) == XR_XIR_VALUE_OK);
    }
    int64_t length = -1;
    CHECK(xr_xir_hash_len(&map, admission, &length) == XR_XIR_VALUE_OK && length == 512);
    XirHash *storage = (XirHash *)object_pointer(&map);
    CHECK(storage->stride == 24 && storage->key_offset == 8 && storage->value_offset == 16);
    int64_t root = map.payload; size_t before = calls; fail_at = calls;
    XrXirValue key = integer(3), value = integer(-1);
    CHECK(xr_xir_map_set(&place, &key, &value, admission) == XR_XIR_VALUE_OK && map.payload == root && calls == before);
    fail_at = SIZE_MAX;
    CHECK(xr_xir_value_copy(&map, &snapshot) == XR_XIR_VALUE_OK);
    value.payload = -2;
    CHECK(xr_xir_map_set(&place, &key, &value, admission) == XR_XIR_VALUE_OK && map.payload != snapshot.payload);
    hash_expect(&snapshot, 3, -1, true, admission); hash_expect(&map, 3, -2, true, admission);
    bool present[512]; for (unsigned i = 0; i < 512; ++i) present[i] = true;
    for (unsigned step = 0; step < 512; ++step) {
        unsigned removed_key = (step * 173u) & 511u;
        key = integer(removed_key); bool removed = false;
        CHECK(xr_xir_hash_delete(&place, &key, admission, &removed) == XR_XIR_VALUE_OK && removed);
        present[removed_key] = false;
        CHECK(xr_xir_hash_len(&map, admission, &length) == XR_XIR_VALUE_OK && length == 511 - step);
        for (unsigned i = 0; i < 512; ++i)
            hash_expect(&map, i, i == 3 ? -2 : (int64_t)i * 7, present[i], admission);
    }
    hash_expect(&snapshot, 511, 3577, true, admission);
    CHECK(xr_xir_hash_clear(&place, admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&snapshot); xr_xir_value_drop(&map);
}
static void hash_ieee_cases(XrXirValueAdmission *admission) {
    XrXirValue elements[] = {{XR_XIR_F64, 0, INT64_C(0x7ff8000000000000)},
        {XR_XIR_F64, 0, (int64_t)UINT64_C(0xfff0000000000001)},
        {XR_XIR_F64, 0, 0}, {XR_XIR_F64, 0, INT64_MIN}};
    XrXirValue set = {0}; int64_t length = -1;
    CHECK(xr_xir_set_new((XrXirType)257, elements, 4, admission, &set) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_hash_len(&set, admission, &length) == XR_XIR_VALUE_OK && length == 2);
    for (unsigned i = 0; i < 4; ++i) {
        bool found = false;
        CHECK(xr_xir_set_contains(&set, &elements[i], admission, &found) == XR_XIR_VALUE_OK && found);
    }
    XrXirValuePlace place = {(XrXirType)257, &set.payload}; bool removed = false;
    CHECK(xr_xir_hash_delete(&place, &elements[1], admission, &removed) == XR_XIR_VALUE_OK && removed);
    CHECK(xr_xir_set_contains(&set, &elements[0], admission, &removed) == XR_XIR_VALUE_OK && !removed);
    CHECK(xr_xir_set_add(&place, &elements[0], admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&set);
}
static void hash_order_cases(XrXirValueAdmission *admission) {
    XrXirValue keys[] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2}, {XR_XIR_I64, 0, 3}};
    XrXirValue values[] = {{XR_XIR_I64, 0, 7}, {XR_XIR_I64, 0, 14}, {XR_XIR_I64, 0, 21}};
    for (unsigned operation = 0; operation < 3; ++operation) {
        XrXirValue map = {0}, alias = {0};
        CHECK(xr_xir_map_new((XrXirType)256, keys, values, 3, admission, &map) == XR_XIR_VALUE_OK);
        XrXirValuePlace place = {(XrXirType)256, &map.payload};
        if (operation) CHECK(xr_xir_value_copy(&map, &alias) == XR_XIR_VALUE_OK);
        if (operation == 1) {
            XrXirValue replacement = integer(91);
            CHECK(xr_xir_map_set(&place, &keys[1], &replacement, admission) == XR_XIR_VALUE_OK);
        } else {
            bool removed = false;
            CHECK(xr_xir_hash_delete(&place, &keys[0], admission, &removed) == XR_XIR_VALUE_OK && removed);
        }
        XirHash *storage = (XirHash *)object_pointer(&map);
        const int64_t *expected = operation == 1 ? (int64_t[]){1,2,3} : (int64_t[]){2,3};
        for (size_t i = 0; i < storage->length; ++i) {
            XrXirValue stored = hash_test_key(storage, i);
            CHECK(stored.payload == expected[i]);
        }
        if (operation != 1) {
            CHECK(xr_xir_map_set(&place, &keys[0], &values[0], admission) == XR_XIR_VALUE_OK);
            storage = (XirHash *)object_pointer(&map);
            const int64_t reinserted[] = {2,3,1};
            CHECK(storage->length == 3);
            for (size_t i = 0; i < storage->length; ++i)
                CHECK(hash_test_key(storage, i).payload == reinserted[i]);
        }
        if (alias.type) {
            XirHash *original = (XirHash *)object_pointer(&alias);
            for (size_t i = 0; i < original->length; ++i)
                CHECK(hash_test_key(original, i).payload == (int64_t)i + 1);
        }
        xr_xir_value_drop(&alias); xr_xir_value_drop(&map);
    }
}
static void hash_string_cases(XrXirValueAdmission *admission) {
    XrXirDomain *producer = NULL; XrXirValue keys[3] = {{0}}, values[3] = {{0}}, map = {0}, alias = {0};
    CHECK(xr_xir_domain_new(65536, &producer) == XR_XIR_VALUE_OK);
    const char *texts[] = {"a\0\xce\xa9", "a\0\xce\xa9", "a\0\xce\xb2"};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(xr_xir_string_new(producer, texts[i], 4, &keys[i]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(producer, "value", i + 1, &values[i]) == XR_XIR_VALUE_OK);
    }
    CHECK(xr_xir_map_new((XrXirType)258, keys, values, 3, admission, &map) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map, &alias) == XR_XIR_VALUE_OK);
    xr_xir_domain_close(producer); xr_xir_domain_drop(producer);
    XrXirValue output = {0}; bool found = false; int64_t length = -1;
    CHECK(xr_xir_hash_len(&map, admission, &length) == XR_XIR_VALUE_OK && length == 2);
    CHECK(xr_xir_map_get(&map, &keys[0], admission, &output, &found) == XR_XIR_VALUE_OK && found);
    CHECK(output.payload == values[1].payload); xr_xir_value_drop(&output);
    XrXirValuePlace place = {(XrXirType)258, &map.payload};
    CHECK(xr_xir_hash_delete(&place, &keys[0], admission, &found) == XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_map_get(&alias, &keys[1], admission, &output, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_hash_clear(&place, admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&alias); xr_xir_value_drop(&map);
    for (unsigned i = 0; i < 3; ++i) { xr_xir_value_drop(&keys[i]); xr_xir_value_drop(&values[i]); }
    XrXirValue unit = {0}, key = integer(42), unit_map = {0};
    CHECK(xr_xir_map_new((XrXirType)261, &key, &unit, 1, admission, &unit_map) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_get(&unit_map, &key, admission, &output, &found) == XR_XIR_VALUE_OK && found && !output.type);
    xr_xir_value_drop(&unit_map);
}
#include "p09_hash_faults.h"
#include "p09_hash_graph.h"
#include "p09_hash_receiving_faults.h"
int main(int argc, char **argv) {
    XrXirTypeArena *arena = hash_test_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000000, 1048576};
    hash_dense_cases(&admission); hash_ieee_cases(&admission); hash_string_cases(&admission); hash_order_cases(&admission);
    if (argc == 2 && !strcmp(argv[1], "full")) hash_faults(arena, domain);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    hash_graph_cases();
    hash_receiving_faults();
    puts("Map/Set dense probe/delete, shared snapshots, IEEE keys, full bytes and final physical0 PASS");
    return 0;
}
