/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void map_literal_semantics(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *a = map_key(&f, 0, "a\0x", 3), *equal = map_key(&f, 1, "a\0x", 3);
    XrtStringBacking *b = map_key(&f, 2, "b", 1), *c = map_key(&f, 3, "c", 1);
    XrtStringBacking *keys[] = {equal, b}; int64_t values[] = {8, 9};
    OK(xrt_map_backing_create(f.frame, 4, &f.types, keys, values, 2));
    const XrtMapBacking *spread = (const XrtMapBacking *)borrow(&f, 4);
    XrtMapLiteralOrigin origins[] = {{0, 10, 3}, {1, 11, 7}, {0, 12, 9}};
    XrtMapLiteralInput inputs[] = {{{.key=a}, 1}, {{.spread=spread}, 0}, {{.key=b}, 3}};
    XrtMapLiteral literal = {"map.xr", 6, origins, 3};
    XrtMapLiteralDuplicate duplicate = {71, 73};
    OK(xrt_map_backing_literal_create(f.frame, 5, &f.types, &literal, inputs, &duplicate));
    CHECK(duplicate.first == 71 && duplicate.second == 73);
    const XrtMapBacking *map = (const XrtMapBacking *)borrow(&f, 5);
    XrtStringBacking *ordered[] = {a, b}; map_check(map, ordered, 2);
    map_value(map, a, true, 8); map_value(map, b, true, 3);
    inputs[0].source.key = c; inputs[1].source.spread = map;
    OK(xrt_map_backing_literal_create(f.frame, 5, &f.types, &literal, inputs, &duplicate));
    map = (const XrtMapBacking *)borrow(&f, 5);
    XrtStringBacking *self_order[] = {c, a, b}; map_check(map, self_order, 3);
    map_value(map, c, true, 1); map_value(map, a, true, 8); map_value(map, b, true, 3);
    inputs[0].source.key = a; inputs[1].source.spread = spread;
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    int32_t a_refs = xr_signed_rc_read(&a->managed.references);
    int32_t spread_refs = xr_signed_rc_read(&spread->managed.references);
    inputs[2].source.key = equal;
    CHECK(xrt_map_backing_literal_create(f.frame, 5, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_DUPLICATE_KEY);
    CHECK(duplicate.first == 0 && duplicate.second == 2 && borrow(&f, 5) == &map->managed);
    CHECK(xr_signed_rc_read(&a->managed.references) == a_refs &&
        xr_signed_rc_read(&spread->managed.references) == spread_refs);
    same_bill(f.billing, before);
    // Two spreads overwrite in written order, retaining the first insertion position.
    keys[0] = c; keys[1] = a; values[0] = 13; values[1] = 12;
    OK(xrt_map_backing_create(f.frame, 6, &f.types, keys, values, 2));
    origins[0].kind = 1; inputs[0].source.spread = spread;
    inputs[1].source.spread = (const XrtMapBacking *)borrow(&f, 6);
    literal.count = 2;
    OK(xrt_map_backing_literal_create(f.frame, 5, &f.types, &literal, inputs, &duplicate));
    map = (const XrtMapBacking *)borrow(&f, 5);
    XrtStringBacking *spread_order[] = {equal, b, c}; map_check(map, spread_order, 3);
    map_value(map, a, true, 12); map_value(map, b, true, 9); map_value(map, c, true, 13);
    literal = (XrtMapLiteral){0};
    OK(xrt_map_backing_literal_create(f.frame, 5, &f.types, &literal, NULL, &duplicate));
    map = (const XrtMapBacking *)borrow(&f, 5);
    CHECK(!map->capacity && !map->length && !map->order_count);
    fixture_done(&f);
}

static void map_literal_rejections(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *a = map_key(&f, 0, "a", 1), *b = map_key(&f, 1, "b", 1);
    XrtStringBacking *keys[] = {a, b}; int64_t values[] = {1, 2};
    OK(xrt_map_backing_create(f.frame, 2, &f.types, keys, values, 2));
    XrtManagedHeader *old = borrow(&f, 2);
    XrtMapLiteralOrigin origins[] = {{0, 1, 1}, {0, 2, 1}};
    XrtMapLiteralInput inputs[] = {{{.key=a}, 41}, {{.key=b}, 42}};
    XrtMapLiteral literal = {NULL, 0, origins, 2};
    XrtMapLiteralDuplicate duplicate = {19, 23};
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    xrt_billing_owner_fail_after(f.billing, 0);
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(f.billing, SIZE_MAX);
    CHECK(borrow(&f, 2) == old && duplicate.first == 19 && duplicate.second == 23);
    same_bill(f.billing, before);
    int32_t a_refs = xr_signed_rc_read(&a->managed.references), b_refs = xr_signed_rc_read(&b->managed.references);
    for (int limit = 0; limit < 2; ++limit) {
        atomic_store(&b->managed.references, INT32_MAX - limit);
        CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_REFCOUNT);
        CHECK(xr_signed_rc_read(&a->managed.references) == a_refs &&
            xr_signed_rc_read(&b->managed.references) == INT32_MAX - limit && borrow(&f, 2) == old);
        CHECK(duplicate.first == 19 && duplicate.second == 23); same_bill(f.billing, before);
        atomic_store(&b->managed.references, b_refs);
    }
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 2, f.types.map, sizeof(XrtMapBacking), &pending));
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    CHECK(borrow(&f, 2) == old && pending->ops == f.types.map);
    OK(xrt_managed_slot_commit(f.frame, 2));
    CHECK(borrow(&f, 2) == pending); // The pre-existing pending token was preserved.
    origins[0].kind = 2;
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    origins[0].kind = 0; origins[1].line = 0;
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    origins[1].line = 2; literal.path = "x";
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    literal.path_length = 1; literal.count = SIZE_MAX / sizeof(*inputs) + 1;
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_OVERFLOW);
    literal.count = SIZE_MAX / sizeof(*origins) + 1;
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_OVERFLOW);
    literal.count = SIZE_MAX / sizeof(*inputs);
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_OVERFLOW);
    literal.count = 2;
    OK(xrt_array_backing_u8_create(f.frame, 3, &f.types, NULL, 0, 0));
    origins[0].kind = 1; inputs[0].source.spread = (const XrtMapBacking *)borrow(&f, 3);
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    origins[0].kind = 0; inputs[0].source.key = (const XrtStringBacking *)borrow(&f, 3);
    BAD(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    CHECK(duplicate.first == 19 && duplicate.second == 23);
    origins[0].kind = 0; inputs[0].source.key = a;
    xrt_billing_owner_close(f.billing);
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_CLOSED);
    CHECK(borrow(&f, 2) == pending && duplicate.first == 19 && duplicate.second == 23);
    fixture_done(&f);
    Fixture limited; fixture_init(&limited, 16384);
    a = map_key(&limited, 0, "a", 1);
    XrtMapLiteralOrigin many_origins[256]; XrtMapLiteralInput many_inputs[256];
    for (size_t n = 0; n < 256; ++n) {
        many_origins[n] = (XrtMapLiteralOrigin){0, (uint32_t)n + 1, 1};
        many_inputs[n] = (XrtMapLiteralInput){{.key=a}, 42};
    }
    literal = (XrtMapLiteral){NULL, 0, many_origins, 256};
    before = xrt_billing_owner_stats(limited.billing);
    CHECK(xrt_map_backing_literal_create(limited.frame, 1, &limited.types, &literal,
        many_inputs, &duplicate) == XRT_MANAGED_LIMIT);
    CHECK(!borrow(&limited, 1) && duplicate.first == 19 && duplicate.second == 23);
    same_bill(limited.billing, before); fixture_done(&limited);
}

typedef struct MapLiteralWatch {
    Fixture *destination, *source;
    XrtManagedSlotLocation slot;
    XrtManagedHeader *old;
    const XrtStringBacking *key;
    bool cleared;
    size_t allocations, retirements;
} MapLiteralWatch;
static void map_literal_observe(void *context, void *memory, bool allocating) {
    MapLiteralWatch *watch = context;
    XrtManagedHeader *published = NULL;
    OK(xrt_managed_slot_borrow(watch->destination->frame, 0, &published));
    CHECK(published == *watch->slot.owner);
    BAD(xrt_managed_root_drain(watch->destination->root));
    XrtMapLiteralDuplicate duplicate = {31, 37};
    const XrtMapLiteral empty = {0};
    BAD(xrt_map_backing_literal_create(watch->destination->frame, 0, &watch->destination->types,
        &empty, NULL, &duplicate));
    CHECK(duplicate.first == 31 && duplicate.second == 37);
    if (allocating) {
        ++watch->allocations;
        CHECK(*watch->slot.owner == watch->old && !watch->cleared);
        watch->cleared = true;
        OK(xrt_managed_root_drain(watch->source->root));
    } else {
        ++watch->retirements;
        if (memory == watch->old) {
            CHECK(*watch->slot.owner != watch->old);
            map_value((const XrtMapBacking *)*watch->slot.owner, watch->key, true, 42);
        }
    }
}
static void map_literal_callback_lifetimes(void) {
    Fixture source, destination; fixture_init(&source, SIZE_MAX); fixture_init(&destination, SIZE_MAX);
    XrtStringBacking *key = map_key(&source, 0, "key", 3);
    int64_t initial = 41;
    OK(xrt_map_backing_create(source.frame, 1, &source.types, &key, &initial, 1));
    const XrtMapBacking *spread = (const XrtMapBacking *)borrow(&source, 1);
    OK(xrt_map_backing_create(destination.frame, 0, &destination.types, NULL, NULL, 0));
    XrtMapLiteralOrigin origins[] = {{1, 1, 1}, {0, 2, 1}};
    XrtMapLiteralInput inputs[] = {{{.spread=spread}, 0}, {{.key=key}, 42}};
    XrtMapLiteral literal = {NULL, 0, origins, 2};
    MapLiteralWatch watch = {.destination=&destination, .source=&source,
        .old=borrow(&destination, 0), .key=key};
    OK(xrt_managed_slot_location(destination.frame, 0, &watch.slot));
    destination.audit.observe = map_literal_observe; destination.audit.observer = &watch;
    source.audit.observe = map_literal_observe; source.audit.observer = &watch;
    XrtMapLiteralDuplicate duplicate = {43, 47};
    OK(xrt_map_backing_literal_create(destination.frame, 0, &destination.types, &literal, inputs, &duplicate));
    source.audit.observe = destination.audit.observe = NULL;
    CHECK(watch.cleared && watch.allocations == 1 && watch.retirements >= 2 &&
        duplicate.first == 43 && duplicate.second == 47);
    XrtManagedHeader *owned = NULL;
    OK(xrt_managed_slot_take(destination.frame, 0, &owned));
    fixture_retire(&source); fixture_retire(&destination);
    CHECK(!atomic_load(&source.audit.finalized) && !atomic_load(&destination.audit.finalized));
    map_value((const XrtMapBacking *)owned, key, true, 42);
    xrt_managed_drop(owned);
    CHECK(atomic_load(&source.audit.finalized) == 1 && atomic_load(&destination.audit.finalized) == 1);
}

typedef struct MapLiteralLargeWatch {
    Fixture *fixture;
    const XrtMapBacking *spread;
    const XrtStringBacking *key;
    XrtManagedHeader *old;
    XrtManagedSlotLocation slot;
    size_t allocations, retirements;
} MapLiteralLargeWatch;
static void map_literal_large_observe(void *context, void *memory, bool allocating) {
    MapLiteralLargeWatch *watch = context;
    BAD(xrt_managed_root_drain(watch->fixture->root));
    if (allocating) {
        ++watch->allocations;
        CHECK(*watch->slot.owner == watch->old &&
            xr_signed_rc_read(&watch->spread->managed.references) == 65537 &&
            xr_signed_rc_read(&watch->key->managed.references) == 2);
    } else if (memory == watch->old) {
        ++watch->retirements;
        CHECK(*watch->slot.owner != watch->old);
        map_value((const XrtMapBacking *)*watch->slot.owner, watch->key, true, 42);
    }
}
static void map_literal_large_inputs(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *key = map_key(&f, 0, "answer", 6);
    OK(xrt_map_backing_create(f.frame, 1, &f.types, NULL, NULL, 0));
    OK(xrt_map_backing_create(f.frame, 2, &f.types, NULL, NULL, 0));
    const XrtMapBacking *spread = (const XrtMapBacking *)borrow(&f, 1);
    const size_t count = 65537;
    XrtMapLiteralOrigin *origins = xr_calloc(count, sizeof(*origins));
    XrtMapLiteralInput *inputs = xr_calloc(count, sizeof(*inputs));
    CHECK(origins && inputs);
    for (size_t n = 0; n + 1 < count; ++n) {
        origins[n] = (XrtMapLiteralOrigin){1, 1, 1};
        inputs[n].source.spread = spread;
    }
    origins[count - 1] = (XrtMapLiteralOrigin){0, 2, 1};
    inputs[count - 1] = (XrtMapLiteralInput){{.key=key}, 42};
    XrtMapLiteral literal = {NULL, 0, origins, count};
    XrtMapLiteralDuplicate duplicate = {53, 59};
    MapLiteralLargeWatch watch = {.fixture=&f, .spread=spread, .key=key, .old=borrow(&f, 2)};
    OK(xrt_managed_slot_location(f.frame, 2, &watch.slot));
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    f.audit.observe = map_literal_large_observe; f.audit.observer = &watch;
    xrt_billing_owner_fail_after(f.billing, 0);
    CHECK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate) == XRT_MANAGED_OOM);
    CHECK(borrow(&f, 2) == watch.old && !watch.allocations && !watch.retirements &&
        xr_signed_rc_read(&spread->managed.references) == 1 && xr_signed_rc_read(&key->managed.references) == 1 &&
        duplicate.first == 53 && duplicate.second == 59);
    same_bill(f.billing, before);
    xrt_billing_owner_fail_after(f.billing, SIZE_MAX);
    OK(xrt_map_backing_literal_create(f.frame, 2, &f.types, &literal, inputs, &duplicate));
    f.audit.observe = NULL;
    CHECK(watch.allocations == 1 && watch.retirements == 1 &&
        xr_signed_rc_read(&spread->managed.references) == 1 && xr_signed_rc_read(&key->managed.references) == 2 &&
        duplicate.first == 53 && duplicate.second == 59);
    const XrtMapBacking *map = (const XrtMapBacking *)borrow(&f, 2);
    CHECK(map->length == 1); map_value(map, key, true, 42);
    XrtManagedHeader *owned = NULL; OK(xrt_managed_slot_take(f.frame, 2, &owned));
    xr_free(inputs); xr_free(origins);
    fixture_retire(&f);
    CHECK(!atomic_load(&f.audit.finalized));
    map_value((const XrtMapBacking *)owned, key, true, 42);
    xrt_managed_drop(owned);
    CHECK(atomic_load(&f.audit.finalized) == 1 && !atomic_load(&f.audit.bytes) && !atomic_load(&f.audit.blocks));
}
