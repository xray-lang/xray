/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_cache_cases.h - Canonical offsets and allocation-free sealed queries
 */
#ifndef XIR_STORAGE_CACHE_CASES_H
#define XIR_STORAGE_CACHE_CASES_H
static void storage_cache_cases(void) {
    XrXirNominalVariant variants[] = {
        {{"Empty", 5}, 0, 0}, {{"Small", 5}, 0, 2}, {{"Text", 4}, 2, 1}};
    XrXirNominalFieldIdentity fields[] = {{{"a", 1}, 0}, {{"b", 1}, 0}, {{"c", 1}, 0}};
    XrXirNominalIdentity identities[] = {
        {{"alpha", 5}, {"Choice", 6}, 1, 0, fields, 3, XR_XIR_NOMINAL_ENUM, variants, 3},
        {{"alpha", 5}, {"Outer", 5}, 1, 0, fields, 3, XR_XIR_NOMINAL_STRUCT, NULL, 0},
        {{"alpha", 5}, {"Empty", 5}, 1, 0, NULL, 0, XR_XIR_NOMINAL_STRUCT, NULL, 0}};
    XrXirNominalTable table = {NULL, 3, identities};
    XrXirType inner[] = {XR_XIR_I8, XR_XIR_BOOL, XR_XIR_STRING};
    XrXirType outer[] = {XR_XIR_BOOL, (XrXirType)256, XR_XIR_I16};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, inner, 3}},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {1, NULL, 0, outer, 3}},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {2, NULL, 0, NULL, 0}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING}};
    XrXirTypes types = {nodes, 4, &table};
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(!live && xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirBudget initial = (XrXirBudget) {.parameters = 65536, .metadata_bytes = 1048576, .scratch_bytes = 1048576, .work = 16777216}, budget = initial;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    uint64_t metadata = initial.metadata_bytes - budget.metadata_bytes;
    uint64_t work = initial.work - budget.work;
    CHECK(budget.scratch_bytes == initial.scratch_bytes);
    xr_xir_type_arena_drop(arena); arena = NULL;
    size_t baseline = live; uint64_t bytes = domain->stats.live_bytes;
    for (unsigned mode = 0; mode < 3; ++mode) {
        budget = initial;
        if (!mode) budget.scratch_bytes = 4 * (sizeof(NominalStorageNode) + sizeof(uint32_t)) - 1;
        else if (mode == 1) budget.work = work - 1;
        else budget.metadata_bytes = metadata - 1;
        XrXirBudget saved = budget;
        CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT && !arena);
        CHECK(!memcmp(&budget, &saved, sizeof(budget)) && live == baseline && domain->stats.live_bytes == bytes);
    }
    budget = initial; budget.metadata_bytes = metadata; budget.work = work;
    budget.scratch_bytes = 4 * (sizeof(NominalStorageNode) + sizeof(uint32_t));
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    CHECK(!budget.metadata_bytes && !budget.work && domain->stats.live_bytes == bytes + metadata);
    memset(nodes, 0xCC, sizeof(nodes)); memset(inner, 0xCC, sizeof(inner));
    memset(outer, 0xCC, sizeof(outer)); memset(identities, 0xCC, sizeof(identities));
    memset(fields, 0xCC, sizeof(fields)); memset(variants, 0xCC, sizeof(variants));
    size_t allocations = calls; fail_at = calls;
    const XrXirStorageLayout *choice = xr_xir_type_arena_storage(arena, (XrXirType)256);
    const XrXirStorageLayout *box = xr_xir_type_arena_storage(arena, (XrXirType)257);
    const XrXirStorageLayout *empty = xr_xir_type_arena_storage(arena, (XrXirType)258);
    CHECK(choice && choice->value.size == 16 && choice->value.alignment == 8 && choice->field_count == 3);
    CHECK(choice->field_offsets[0] == 8 && choice->field_offsets[1] == 9 && choice->field_offsets[2] == 8);
    CHECK(box && box->value.size == 32 && box->value.alignment == 8 && box->field_count == 3);
    CHECK(box->field_offsets[0] == 0 && box->field_offsets[1] == 8 && box->field_offsets[2] == 24);
    CHECK(empty && !empty->value.size && empty->value.alignment == 1 && !empty->field_offsets);
    XrXirLayout layout = {0};
    CHECK(xr_xir_type_arena_layout(arena, (XrXirType)259, &layout) && layout.size == 8 && layout.alignment == 8);
    CHECK(xr_xir_type_arena_layout(arena, XR_XIR_I16, &layout) && layout.size == 2 && layout.alignment == 2);
    CHECK(xr_xir_type_arena_layout(arena, XR_XIR_UNIT, &layout) && !layout.size && layout.alignment == 1);
    CHECK(!xr_xir_type_arena_layout(arena, (XrXirType)260, &layout) && !layout.size && !layout.alignment);
    CHECK(!xr_xir_type_arena_storage(NULL, (XrXirType)256));
    CHECK(!xr_xir_type_arena_storage(arena, XR_XIR_I64));
    CHECK(!xr_xir_type_arena_layout(arena, XR_XIR_I64, NULL));
    xr_xir_domain_drop(domain);
    CHECK(xr_xir_type_arena_layout(arena, (XrXirType)257, &layout) && layout.size == 32);
    xr_xir_type_arena_drop(arena);
    CHECK(!live && calls == allocations); fail_at = SIZE_MAX;
}
static void storage_cache_shared_graph(void) {
    enum { COUNT = 160 };
    XrXirNominalFieldIdentity fields[] = {{{"a", 1}, 0}, {{"b", 1}, 0}};
    XrXirNominalIdentity identities[COUNT] = {{0}}; char names[COUNT][16];
    XrXirNominalTable table = {NULL, COUNT, identities};
    XrXirType inner[COUNT + 1]; XrXirTypeNode nodes[COUNT] = {{0}};
    for (uint32_t i = 0; i < COUNT; ++i) {
        int length = snprintf(names[i], sizeof(names[i]), "T%u", i);
        CHECK(length > 0 && (size_t)length < sizeof(names[i]));
        identities[i] = (XrXirNominalIdentity) {{"alpha", 5}, {names[i], (uint32_t)length},
            1, 0, fields, i ? 1u : 2u, XR_XIR_NOMINAL_STRUCT, NULL, 0};
        nodes[i].nominal.declaration = i;
        inner[i] = i + 1 == COUNT ? XR_XIR_STRING : (XrXirType)(257 + i);
        nodes[i].kind = XR_XIR_TYPE_NOMINAL;
        nodes[i].nominal.fields = &inner[i]; nodes[i].nominal.field_count = 1;
    }
    /* The root contains the same completed subtree twice. */
    XrXirType roots[] = {(XrXirType)257, (XrXirType)257};
    nodes[0].nominal.fields = roots; nodes[0].nominal.field_count = 2;
    XrXirTypes types = {nodes, COUNT, &table}; XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(!live && xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirBudget budget = (XrXirBudget) {.parameters = 65536, .metadata_bytes = 1048576, .scratch_bytes = 1048576, .work = 16777216};
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    const XrXirStorageLayout *root = xr_xir_type_arena_storage(arena, (XrXirType)256);
    CHECK(root && root->value.size == 16 && root->field_offsets[0] == 0 && root->field_offsets[1] == 8);
    for (uint32_t i = 1; i < COUNT; ++i) {
        const XrXirStorageLayout *layout = xr_xir_type_arena_storage(arena, (XrXirType)(256 + i));
        CHECK(layout && layout->value.size == 8 && layout->value.alignment == 8 && layout->field_offsets[0] == 0);
    }
    xr_xir_type_arena_drop(arena); arena = NULL;
    inner[COUNT - 1] = (XrXirType)256; budget = (XrXirBudget) {.parameters = 65536, .metadata_bytes = 1048576, .scratch_bytes = 1048576, .work = 16777216};
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_BAD_ARGUMENT && !arena);
    xr_xir_domain_drop(domain); CHECK(!live);
}
#endif // XIR_STORAGE_CACHE_CASES_H
