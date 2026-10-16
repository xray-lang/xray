/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_contains.c - Key membership without extracting inline values
 */
#define XR_COMPILE_DOMAIN_ENTRY original_contains_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#include "xir/xxir_map.h"
static XrXirValue integer(int64_t value) { return (XrXirValue){XR_XIR_I64, 0, value}; }
static XrXirTypeArena *contains_arena(void) {
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NULLABLE, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = (XrXirType)256},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_STRING, .result = (XrXirType)256},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_F64, .result = (XrXirType)256}
    };
    XrXirTypes types = {nodes, 5, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 100), &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void contains_bad_admission(const XrXirValue *map, const XrXirValue *set,
    const XrXirValue *key, const XrXirValue *value, XrXirValueAdmission *admission) {
    XrXirValueAdmission no_arena = *admission, no_domain = *admission;
    no_arena.arena = NULL; no_domain.domain = NULL;
    XrXirValueAdmission *invalid[] = {NULL, &no_arena, &no_domain};
    XrXirValue root = *map, set_root = *set, output = {0};
    XrXirValuePlace place = {(XrXirType)root.type, &root.payload};
    XrXirValuePlace set_place = {(XrXirType)set_root.type, &set_root.payload};
    size_t before = calls;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        bool found = true; int64_t length = -1;
        uint64_t work = invalid[i] ? invalid[i]->work : 0;
        size_t scratch = invalid[i] ? invalid[i]->scratch_bytes : 0;
        CHECK(xr_xir_map_new((XrXirType)map->type, key, value, 1, invalid[i], &output) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_set_new((XrXirType)set->type, key, 1, invalid[i], &output) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_map_get(map, key, invalid[i], &output, &found) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_map_contains_key(map, key, invalid[i], &found) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_set_contains(set, key, invalid[i], &found) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_hash_len(map, invalid[i], &length) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_map_set(&place, key, value, invalid[i]) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_set_add(&set_place, key, invalid[i]) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_hash_delete(&place, key, invalid[i], &found) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_hash_clear(&place, invalid[i]) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(found && length == -1 && !output.type && !output.payload && calls == before);
        CHECK(root.payload == map->payload && set_root.payload == set->payload);
        CHECK(!invalid[i] || (invalid[i]->work == work && invalid[i]->scratch_bytes == scratch));
    }
}
static void contains_cases(XrXirValueAdmission *admission, bool full) {
    XrXirValue text = {0}, optional = {0}, map = {0}, alias = {0}, set = {0};
    CHECK(xr_xir_string_new(admission->domain, "value", 5, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)256, &text, admission, &optional) == XR_XIR_VALUE_OK);
    XrXirValue key = integer(42), missing = integer(7);
    CHECK(xr_xir_map_new((XrXirType)257, &key, &optional, 1, admission, &map) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map, &alias) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)258, &key, 1, admission, &set) == XR_XIR_VALUE_OK);
    contains_bad_admission(&map, &set, &key, &optional, admission);
    /* Extracting this inline Nullable allocates its independently owned box.
     * A key-only query must not need that result or its allocation. */
    size_t before = calls; fail_at = calls;
    XrXirValue output = {0}; bool found = false;
    CHECK(xr_xir_map_get(&map, &key, admission, &output, &found) == XR_XIR_VALUE_OOM);
    CHECK(!output.type && !output.payload && !found && calls == before + 1);
    fail_at = calls; before = calls;
    CHECK(xr_xir_map_contains_key(&map, &key, admission, &found) == XR_XIR_VALUE_OK && found);
    CHECK(calls == before);
    found = true;
    CHECK(xr_xir_map_contains_key(&map, &missing, admission, &found) == XR_XIR_VALUE_OK && !found && calls == before);
    fail_at = SIZE_MAX;
    CHECK(xr_xir_map_get(&map, &key, admission, &output, &found) == XR_XIR_VALUE_OK && found);
    CHECK(output.type == optional.type && output.payload != optional.payload);
    xr_xir_value_drop(&output);
    XirHash *storage = (XirHash *)object_pointer(&map);
    size_t size = 0; CHECK(hash_data_size(storage->capacity, storage->stride, &size));
    unsigned char bytes[512]; CHECK(size <= sizeof(bytes)); memcpy(bytes, storage->data, size);
    int64_t root = map.payload; unsigned char *data = storage->data;
    admission->work = 1000000; uint64_t start = admission->work;
    CHECK(xr_xir_map_contains_key(&map, &key, admission, &found) == XR_XIR_VALUE_OK && found);
    uint64_t needed = start - admission->work; CHECK(needed > 1);
    uint64_t first = full ? 0 : needed - 1;
    for (uint64_t work = first; work <= needed; ++work) {
        admission->work = work; found = false; before = calls;
        CHECK(xr_xir_map_contains_key(&map, &key, admission, &found) ==
            (work == needed ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
        CHECK(found == (work == needed) && calls == before);
        CHECK(map.payload == root && alias.payload == root && storage->data == data && !memcmp(data, bytes, size));
    }
    admission->work = 1000000; found = true;
    CHECK(xr_xir_map_contains_key(&set, &key, admission, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    CHECK(xr_xir_set_contains(&map, &key, admission, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    XrXirValue wrong = {XR_XIR_BOOL, 0, 1};
    CHECK(xr_xir_map_contains_key(&map, &wrong, admission, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    CHECK(xr_xir_map_contains_key(&map, &key, NULL, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    CHECK(xr_xir_map_contains_key(&map, &key, admission, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_set_contains(&set, &key, admission, &found) == XR_XIR_VALUE_OK && found);
    printf("Map key-only lookup exact work=%llu, zero requests, %s work boundaries; inline-value get has one real OOM\n",
        (unsigned long long)needed, full ? "all" : "exact/minus-one");
    xr_xir_value_drop(&set); xr_xir_value_drop(&alias); xr_xir_value_drop(&map);
    xr_xir_value_drop(&optional); xr_xir_value_drop(&text);
}
static void contains_key_relations(XrXirValueAdmission *admission) {
    XrXirValue text = {0}, optional = {0}, map = {0}, key = {0}, prefix = {0};
    CHECK(xr_xir_string_new(admission->domain, "v", 1, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)256, &text, admission, &optional) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(admission->domain, "a\0b", 3, &key) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(admission->domain, "a", 1, &prefix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)259, &key, &optional, 1, admission, &map) == XR_XIR_VALUE_OK);
    bool found = false;
    CHECK(xr_xir_map_contains_key(&map, &key, admission, &found) == XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_map_contains_key(&map, &prefix, admission, &found) == XR_XIR_VALUE_OK && !found);
    xr_xir_value_drop(&map); xr_xir_value_drop(&key); xr_xir_value_drop(&prefix);
    XrXirValue zero = {XR_XIR_F64, 0, INT64_MIN}, nan = {XR_XIR_F64, 0, INT64_C(0x7ff8000000000000)};
    CHECK(xr_xir_map_new((XrXirType)260, &zero, &optional, 1, admission, &map) == XR_XIR_VALUE_OK);
    XrXirValue positive_zero = {XR_XIR_F64, 0, 0};
    CHECK(xr_xir_map_contains_key(&map, &positive_zero, admission, &found) == XR_XIR_VALUE_OK && found);
    XrXirValuePlace place = {(XrXirType)260, &map.payload};
    CHECK(xr_xir_map_set(&place, &nan, &optional, admission) == XR_XIR_VALUE_OK);
    nan.payload = INT64_C(0x7ff0000000000001);
    CHECK(xr_xir_map_contains_key(&map, &nan, admission, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&map); xr_xir_value_drop(&optional); xr_xir_value_drop(&text);
}
static void contains_function_authority(void) {
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = (XrXirType)257}
    };
    XrXirTypes types = {nodes, 3, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 100), &arena) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    size_t releases = 0, wrong_owner = 0;
    XrXirValueAdmission admission = {arena, domain, capture_admit, &releases, 1000000, 65536};
    XrXirFunctionBinding binding = {&releases, capture_release, 7, NULL, 0};
    XrXirValue function = {0}, optional = {0}, map = {0}, key = integer(42);
    CHECK(xr_xir_function_new(domain, arena, (XrXirType)256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &function, &admission, &optional) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)258, &key, &optional, 1, &admission, &map) == XR_XIR_VALUE_OK);
    bool found = false;
    CHECK(xr_xir_map_contains_key(&map, &key, &admission, &found) == XR_XIR_VALUE_OK && found);
    admission.function = NULL;
    CHECK(xr_xir_map_contains_key(&map, &key, &admission, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    admission.function = capture_admit; admission.context = &wrong_owner;
    CHECK(xr_xir_map_contains_key(&map, &key, &admission, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    admission.context = &releases;
    CHECK(xr_xir_map_contains_key(&map, &key, &admission, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&map); xr_xir_value_drop(&optional); xr_xir_value_drop(&function);
    CHECK(releases == 1 && !wrong_owner);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
}
static void contains_class_authority(void) {
    XrXirNominalFieldIdentity field = {{"value", 5}, XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {.module = {"contains", 8}, .name = {"Node", 4},
        .fields = &field, .field_count = 1, .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity}; XrXirType field_type = XR_XIR_I64;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {.fields = &field_type, .field_count = 1}},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = (XrXirType)257}
    };
    XrXirTypes types = {nodes, 3, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 100), &arena) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL, *foreign = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576, &foreign) == XR_XIR_VALUE_OK);
    XrXirValueAdmission local = {arena, domain, NULL, NULL, 1000000, 65536}, remote = local;
    remote.domain = foreign;
    XrXirValue node = {0}, optional = {0}, map = {0}, key = integer(42);
    CHECK(xr_xir_class_new((XrXirType)256, &key, 1, &local, &node) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &node, &local, &optional) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)258, &key, &optional, 1, &local, &map) == XR_XIR_VALUE_OK);
    bool found = false;
    CHECK(xr_xir_map_contains_key(&map, &key, &local, &found) == XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_map_contains_key(&map, &key, &remote, &found) == XR_XIR_VALUE_BAD_ARGUMENT && found);
    CHECK(xr_xir_map_contains_key(&map, &key, &local, &found) == XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&map); xr_xir_value_drop(&optional); xr_xir_value_drop(&node);
    xr_xir_compile_type_arena_drop(arena);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    xr_xir_domain_close(foreign); xr_xir_domain_drop(foreign);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
}
int main(int argc, char **argv) {
    XrXirTypeArena *arena = contains_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 1000000, 1048576};
    contains_cases(&admission, argc == 2 && !strcmp(argv[1], "full"));
    contains_key_relations(&admission);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    contains_function_authority();
    contains_class_authority();
    puts("Map/Set kind authority, nullable value independence, key relations and final physical0 PASS");
    return 0;
}
