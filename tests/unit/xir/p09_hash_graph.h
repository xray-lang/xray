/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_graph.h - Real identity edges, foreign detachment and final cycles
 */
#ifndef P09_HASH_GRAPH_H
#define P09_HASH_GRAPH_H
static XrXirTypeArena *hash_graph_arena(void) {
    XrXirNominalFieldIdentity field = {{"link", 4}, XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {.module = {"graph", 5}, .name = {"Node", 4}, .exported = 1,
        .fields = &field, .field_count = 1, .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity}; XrXirType field_type = (XrXirType)258;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {.fields = &field_type, .field_count = 1}},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = (XrXirType)256},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)257}
    };
    XrXirTypes types = {nodes, 3, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(1048576, 1048576, 1000000), &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void hash_graph_cases(void) {
    XrXirTypeArena *arena = hash_graph_arena(); XrXirDomain *owner = NULL, *foreign = NULL;
    CHECK(xr_xir_domain_new(1048576, &owner) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576, &foreign) == XR_XIR_VALUE_OK);
    XrXirValueAdmission local = {arena, owner, NULL, NULL, 1000000, 65536};
    XrXirValueAdmission remote = {arena, foreign, NULL, NULL, 1000000, 65536};
    XrXirValue map = {0}, node = {0}, none = {0}, some = {0}, key = integer(7), other = {0};
    CHECK(xr_xir_map_new((XrXirType)257, NULL, NULL, 0, &remote, &map) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258, NULL, &local, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256, &none, 1, &local, &node) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256, &none, 1, &remote, &other) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)257, &map.payload}; int64_t previous = map.payload;
    CHECK(xr_xir_map_set(&place, &key, &other, &local) == XR_XIR_VALUE_BAD_ARGUMENT && map.payload == previous);
    CHECK(xr_xir_map_set(&place, &key, &node, &local) == XR_XIR_VALUE_OK && map.payload != previous);
    CHECK(object_pointer(&map)->domain == owner);
    CHECK(xr_xir_value_admit(&map, (XrXirType)257, &remote) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_nullable_new((XrXirType)258, &map, &local, &some) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&node, 0, &some, &local) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&map, (XrXirType)257, &local) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&some); xr_xir_value_drop(&none); xr_xir_value_drop(&other);
    xr_xir_domain_close(foreign); xr_xir_domain_drop(foreign);
    xr_xir_domain_close(owner);
    XrXirValue read = {0}; bool found = false;
    CHECK(xr_xir_map_get(&map, &key, &local, &read, &found) == XR_XIR_VALUE_OK && found && read.payload == node.payload);
    xr_xir_value_drop(&read); xr_xir_value_drop(&map);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(owner);
    size_t before = calls; fail_at = calls;
    xr_xir_value_drop(&node); CHECK(calls == before); fail_at = SIZE_MAX;
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Map/Class/Optional cycle, foreign identity rejection, receiving detachment and final no-allocation retirement PASS");
}
#endif // P09_HASH_GRAPH_H
