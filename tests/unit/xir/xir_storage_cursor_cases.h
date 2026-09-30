/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_cursor_cases.h - Active payload traversal and partial-owner release
 */
#ifndef XIR_STORAGE_CURSOR_CASES_H
#define XIR_STORAGE_CURSOR_CASES_H
static XrXirTypeArena *storage_cursor_arena(XrXirDomain *domain) {
    const XrXirNominalVariant variants[] = {{{"Empty", 5}, 0, 0}, {{"Pair", 4}, 0, 2}};
    const XrXirNominalFieldIdentity fields[] = {{{"a", 1}, 0}, {{"b", 1}, 0}};
    const XrXirNominalIdentity identities[] = {
        {{"alpha", 5}, {"Choice", 6}, 1, 0, fields, 2, XR_XIR_NOMINAL_ENUM, variants, 2},
        {{"alpha", 5}, {"Box", 3}, 1, 0, fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0},
        {{"alpha", 5}, {"Empty", 5}, 1, 0, NULL, 0, XR_XIR_NOMINAL_STRUCT, NULL, 0}};
    const XrXirNominalTable table = {NULL, 3, identities};
    const XrXirType strings[] = {XR_XIR_STRING, XR_XIR_STRING};
    const XrXirType choices[] = {(XrXirType)256, (XrXirType)256};
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, strings, 2}},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {1, NULL, 0, choices, 2}},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {2, NULL, 0, NULL, 0}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)257},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258}};
    const XrXirTypes types = {nodes, 6, &table, NULL};
    XrXirBudget budget = {.parameters = 100, .metadata_bytes = 65536, .scratch_bytes = 65536, .work = 10000};
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    const XrXirStorageLayout *choice = xr_xir_type_arena_storage(arena, (XrXirType)256);
    const XrXirStorageLayout *box = xr_xir_type_arena_storage(arena, (XrXirType)257);
    const XrXirStorageLayout *empty = xr_xir_type_arena_storage(arena, (XrXirType)258);
    CHECK(choice->value.size == 24 && choice->depth == 1 && choice->owned_depth == 1 && choice->tag_bytes == 1);
    CHECK(box->value.size == 48 && box->depth == 2 && box->owned_depth == 2 && !box->tag_bytes);
    CHECK(empty->value.size == 0 && empty->depth == 1 && empty->owned_depth == 0);
    return arena;
}
static void storage_cursor_owners(XrXirDomain *domain, unsigned char *bytes) {
    XrXirValue left = {0}, right = {0};
    CHECK(xr_xir_string_new(domain, "left", 4, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "right", 5, &right) == XR_XIR_VALUE_OK);
    bytes[0] = 1;
    memcpy(bytes + 8, &left.payload, sizeof(left.payload));
    memcpy(bytes + 16, &right.payload, sizeof(right.payload));
    /* The initialized raw fields now own both handles. */
    left = (XrXirValue) {0}; right = (XrXirValue) {0};
}
static void storage_cursor_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(!live && xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = storage_cursor_arena(domain);
    uint64_t baseline = domain->stats.live_bytes;
    unsigned char bytes[48]; memset(bytes, 0xFF, sizeof(bytes));
    StorageFrame frames[2]; StorageCursor cursor = {0};
    StorageSpan root = {(XrXirType)257, bytes}, leaf = {0}; bool found = false;
    storage_cursor_owners(domain, bytes); bytes[24] = 0;
    CHECK(storage_cursor_init(arena, root, frames, 1, true, &cursor) == XR_XIR_VALUE_LIMIT);
    CHECK(storage_cursor_init(arena, root, frames, 2, true, &cursor) == XR_XIR_VALUE_OK);
    uint64_t work = 1;
    CHECK(storage_cursor_next(&cursor, &work, &leaf, &found) == XR_XIR_VALUE_LIMIT && !found && !work);
    work = 100;
    CHECK(storage_cursor_next(&cursor, &work, &leaf, &found) == XR_XIR_VALUE_OK && found);
    CHECK(leaf.type == XR_XIR_STRING && leaf.bytes == bytes + 8);
    CHECK(storage_cursor_next(&cursor, &work, &leaf, &found) == XR_XIR_VALUE_OK && found && leaf.bytes == bytes + 16);
    CHECK(storage_cursor_next(&cursor, &work, &leaf, &found) == XR_XIR_VALUE_OK && !found);
    size_t allocations = calls; fail_at = calls; XirObject *pending = NULL;
    CHECK(storage_cursor_init(arena, root, frames, 2, true, &cursor) == XR_XIR_VALUE_OK);
    storage_queue_release(&cursor, UINT64_MAX, &pending); CHECK(pending);
    release_pending(pending); CHECK(calls == allocations && domain->stats.live_bytes == baseline);
    fail_at = SIZE_MAX;
    storage_cursor_owners(domain, bytes); bytes[24] = 255;
    allocations = calls; fail_at = calls; pending = NULL;
    CHECK(storage_cursor_init(arena, root, frames, 2, true, &cursor) == XR_XIR_VALUE_OK);
    storage_queue_release(&cursor, 2, &pending);
    release_pending(pending); CHECK(calls == allocations && domain->stats.live_bytes == baseline);
    /* The suffix tag is uninitialized: rollback above must not inspect it. */
    root = (StorageSpan) {(XrXirType)256, bytes + 24}; work = 100;
    CHECK(storage_cursor_init(arena, root, frames, 2, false, &cursor) == XR_XIR_VALUE_OK);
    CHECK(storage_cursor_next(&cursor, &work, &leaf, &found) == XR_XIR_VALUE_BAD_ARGUMENT && !found);
    root = (StorageSpan) {(XrXirType)258, NULL};
    CHECK(storage_cursor_init(arena, root, NULL, 0, true, &cursor) == XR_XIR_VALUE_OK);
    CHECK(storage_cursor_next(&cursor, NULL, &leaf, &found) == XR_XIR_VALUE_OK && !found);
    CHECK(storage_cursor_init(arena, root, frames, 2, false, &cursor) == XR_XIR_VALUE_OK);
    CHECK(storage_cursor_next(&cursor, NULL, &leaf, &found) == XR_XIR_VALUE_OK && !found);
    CHECK(storage_cursor_init(arena, (StorageSpan){XR_XIR_STRING, NULL}, NULL, 0, true, &cursor) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(calls == allocations);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live && calls == allocations);
    fail_at = SIZE_MAX;
}
#endif // XIR_STORAGE_CURSOR_CASES_H
