/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_view_types.inc.c - Owned native catalogs below exact Array identities
 *
 * KEY CONCEPT:
 *   Every cached directory retains its actual identity owner and resource
 *   ledger. Array contents and canonical loan state remain with their values.
 */
static size_t view_types_position(XirTypeIdentity identity, uint32_t capacity) {
    uint64_t key[] = {(uint64_t)(uintptr_t)identity.owner, (uint32_t)identity.type};
    return xr_hash_bytes(key, sizeof(key)) & (capacity - 1);
}
static XrXirTypeArena *view_types_find(const XrtViewRegistry *registry, XirTypeIdentity identity) {
    if (!registry->type_capacity) return NULL;
    for (size_t at = view_types_position(identity, registry->type_capacity);;
         at = (at + 1) & (registry->type_capacity - 1)) {
        const XrtViewTypeEntry *entry = &registry->type_entries[at];
        if (!entry->arena) return NULL;
        if (entry->identity.owner == identity.owner && entry->identity.type == identity.type)
            return entry->arena;
    }
}
static void view_types_place(XrtViewTypeEntry *entries, uint32_t capacity, XrtViewTypeEntry entry) {
    for (size_t at = view_types_position(entry.identity, capacity);; at = (at + 1) & (capacity - 1)) {
        if (!entries[at].arena) { entries[at] = entry; return; }
    }
}
static void view_types_reserve(XrtViewRegistry *registry, uint32_t source) {
    if (registry->type_count < registry->type_capacity / 2) return;
    uint32_t capacity = registry->type_capacity ? registry->type_capacity * 2 : 8;
    if (capacity < registry->type_capacity || sizeof(XrtViewTypeEntry) > SIZE_MAX / capacity)
        xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    XrtViewTypeEntry *entries = view_allocate(registry, (size_t)capacity * sizeof(*entries), source);
    for (uint32_t n = 0; n < registry->type_capacity; ++n)
        if (registry->type_entries[n].arena) view_types_place(entries, capacity, registry->type_entries[n]);
    if (registry->type_entries) xr_xir_domain_deallocate(registry->root->domain, registry->type_entries,
        (size_t)registry->type_capacity * sizeof(*registry->type_entries));
    registry->type_entries = entries;
    registry->type_capacity = capacity;
}
/* The borrowed result is held by this Root's cache. Physical Cells retain their
 * own reference, including when module storage outlives the current Root. */
static XrXirTypeArena *view_types_get(XrtViewRegistry *registry,
    const XrXirValue *array, uint32_t source) {
    XirTypeIdentity identity = {0};
    const XrXirTypeArena *actual = xr_xir_value_arena(array);
    if (!xir_type_arena_identity(actual, (XrXirType)array->type, &identity))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirTypeArena *known = view_types_find(registry, identity);
    if (known) return known;
    view_types_reserve(registry, source);
    XR_CHECK(!registry->pending_types, "one receiving directory owns every fallible construction");
    if (array->type == VIEW_ARRAY_TYPE && xir_native_type_arena_i64_views(actual)) {
        if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)actual))
            xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
        registry->pending_types = (XrXirTypeArena *)actual;
    } else {
        view_require_status(xir_native_type_arena_derive_i64_views(registry->types,
            array, &registry->pending_types), source);
    }
    XrXirTypeArena *received = registry->pending_types;
    view_types_place(registry->type_entries, registry->type_capacity, (XrtViewTypeEntry){identity, received});
    ++registry->type_count;
    registry->pending_types = NULL;
    return received;
}
