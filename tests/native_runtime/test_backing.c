/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_backing.c - Typed fixed storage, pending rollback and independent leases
 *
 * KEY CONCEPT:
 *   Tests exercise real byte extents and real child references without an XIR
 *   value, guest registry or a linked product-family implementation.
 */
#include "runtime/native/xnative_array_ops.h"
#include "os/os_random.h"
#include "runtime/native/xnative_map_backing.h"
#include "base/xhash.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define BAD(c) CHECK((c) == XRT_MANAGED_ARGUMENT)

typedef struct Audit {
    _Atomic(size_t) leases, bytes, blocks, finalized;
    const XrtMapBacking *watched_map;
    const XrtStringBacking *watched_key;
    bool watched_empty;
    size_t watched_frees;
    unsigned char allocation_fill;
    void (*observe)(void *, void *, bool);
    void *observer;
} Audit;
typedef struct Fixture {
    Audit audit;
    XrtBillingOwner *billing;
    XrtBackingTypes types;
    XrtManagedRoot *root;
    XrtManagedFrame *frame;
} Fixture;

static bool provider_retain(void *context) {
    atomic_fetch_add(&((Audit *)context)->leases, 1);
    return true;
}
static void provider_drop(void *context) {
    Audit *audit = context;
    size_t previous = atomic_fetch_sub(&audit->leases, 1);
    CHECK(previous);
    if (previous == 1) {
        CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks));
        atomic_fetch_add(&audit->finalized, 1);
    }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = context;
    if (audit->observe) audit->observe(audit->observer,NULL,true);
    void *memory = xr_malloc(bytes);
    if (memory) {
        if (audit->allocation_fill) memset(memory, audit->allocation_fill, bytes);
        atomic_fetch_add(&audit->bytes, bytes);
        atomic_fetch_add(&audit->blocks, 1);
    }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
    if (audit->observe) audit->observe(audit->observer,memory,false);
    if (memory == audit->watched_key) {
        const XrtMapBacking *map = audit->watched_map;
        int64_t value = 99; bool found = true;
        OK(xrt_map_backing_get(map, audit->watched_key, &value, &found));
        CHECK(!found && !value);
        for (size_t i = 0; i < map->capacity; ++i) CHECK(map->rows[i].key != audit->watched_key);
        if (audit->watched_empty) CHECK(!map->length && !map->order_count);
        ++audit->watched_frees;
    }
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes);
    CHECK(atomic_fetch_sub(&audit->blocks, 1));
    xr_free(memory);
}
static void billing_new(Audit *audit, size_t limit, XrtBillingOwner **output) {
    *audit = (Audit){0};
    atomic_init(&audit->leases, 1);
    XrtManagedAllocator allocator = {audit, provider_allocate, provider_free,
        {audit, provider_retain, provider_drop}};
    OK(xrt_billing_owner_new(&allocator, limit, output));
}
static void fixture_init(Fixture *fixture, size_t limit) {
    *fixture = (Fixture){0};
    billing_new(&fixture->audit, limit, &fixture->billing);
    OK(xrt_backing_types_init(fixture->billing, &fixture->types));
    OK(xrt_managed_root_new(fixture->billing, &fixture->root));
    OK(xrt_managed_frame_enter(fixture->root, 16, &fixture->frame));
}
static void fixture_retire(Fixture *fixture) {
    OK(xrt_managed_root_destroy(&fixture->root));
    xrt_backing_types_drop(&fixture->types);
    xrt_billing_owner_drop(fixture->billing);
    fixture->billing = NULL;
    provider_drop(&fixture->audit);
}
static void fixture_done(Fixture *fixture) {
    fixture_retire(fixture);
    CHECK(atomic_load(&fixture->audit.finalized) == 1);
    CHECK(!atomic_load(&fixture->audit.bytes) && !atomic_load(&fixture->audit.blocks));
}
static XrtManagedHeader *borrow(Fixture *fixture, size_t slot) {
    XrtManagedHeader *value = NULL;
    OK(xrt_managed_slot_borrow(fixture->frame, slot, &value));
    return value;
}
static XrtManagedHeader *tail(const XrtArrayBacking *array, size_t index) {
    XrtManagedHeader *value = NULL;
    CHECK(index < array->capacity);
    memcpy(&value, array->elements + index * sizeof(value), sizeof(value));
    return value;
}
static void same_bill(XrtBillingOwner *billing, XrtBillingStats before) {
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
}
static void setup_failures(void) {
    Audit audit;
    XrtBillingOwner *billing = NULL;
    billing_new(&audit, SIZE_MAX, &billing);
    XrtBillingStats baseline = xrt_billing_owner_stats(billing);
    for (size_t allowed = 0; allowed < 14; ++allowed) {
        XrtBackingTypes types = {0};
        xrt_billing_owner_fail_after(billing, allowed);
        CHECK(xrt_backing_types_init(billing, &types) == XRT_MANAGED_OOM);
        CHECK(!types.string && !types.map && !types.set);
        for (size_t i = 0; i < XRT_BACKING_ARRAY_COUNT; ++i) CHECK(!types.arrays[i]);
        xrt_backing_types_drop(&types);
        same_bill(billing, baseline);
    }
    xrt_billing_owner_fail_after(billing, 14);
    XrtBackingTypes types = {0};
    OK(xrt_backing_types_init(billing, &types));
    CHECK(xrt_billing_owner_stats(billing).blocks == baseline.blocks + 14 && types.map && types.set);
    XrtManagedOps *stable = types.string;
    BAD(xrt_backing_types_init(billing, &types));
    CHECK(types.string == stable);
    xrt_backing_types_drop(&types);
    xrt_backing_types_drop(&types);
    same_bill(billing, baseline);
    xrt_billing_owner_drop(billing);
    provider_drop(&audit);
    CHECK(atomic_load(&audit.finalized) == 1);
}
static void string_values(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "\303\251\0x", 4, 16));
    XrtStringBacking *first = (XrtStringBacking *)borrow(&fixture, 0);
    CHECK(first->managed.ops == fixture.types.string && first->length == 4 && first->runes == 3);
    CHECK(first->capacity == 16 && !memcmp(first->bytes, "\303\251\0x", 5));
    XrtBillingStats after = xrt_billing_owner_stats(fixture.billing);
    CHECK(after.bytes == before.bytes + sizeof(*first) + 16 && after.blocks == before.blocks + 1);
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, NULL, 0, 0));
    XrtStringBacking *empty = (XrtStringBacking *)borrow(&fixture, 1);
    CHECK(!empty->length && !empty->runes && empty->capacity == 1 && !empty->bytes[0]);
    OK(xrt_backing_slot_copy(fixture.frame, 2, &first->managed));
    CHECK(xr_signed_rc_read(&first->managed.references) == 2);
    BAD(xrt_backing_slot_copy(fixture.frame, 2, &first->managed));
    CHECK(xr_signed_rc_read(&first->managed.references) == 2);
    before = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "\300\200", 2, 0) == XRT_MANAGED_BAD_UTF8);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "\355\240\200", 3, 0) == XRT_MANAGED_BAD_UTF8);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "\303", 1, 0) == XRT_MANAGED_BAD_UTF8);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "x", SIZE_MAX, 0) == XRT_MANAGED_OVERFLOW);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "x", 1, SIZE_MAX) == XRT_MANAGED_OVERFLOW);
    first->runes = SIZE_MAX;
    CHECK(xrt_string_backing_concat_create(fixture.frame, 0, &fixture.types, first, first, 0) == XRT_MANAGED_OVERFLOW);
    first->runes = 3;
    CHECK(borrow(&fixture, 0) == &first->managed);
    same_bill(fixture.billing, before);
    OK(xrt_string_backing_concat_create(fixture.frame, 0, &fixture.types, first, first, 0));
    XrtStringBacking *joined = (XrtStringBacking *)borrow(&fixture, 0);
    CHECK(joined != first && joined->length == 8 && joined->runes == 6 && joined->capacity == 9);
    CHECK(!memcmp(joined->bytes, first->bytes, 4) && !memcmp(joined->bytes + 4, first->bytes, 4));
    CHECK(!joined->bytes[8] && xr_signed_rc_read(&first->managed.references) == 1);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 0, fixture.types.string, sizeof(XrtStringBacking) + 3, &pending));
    XrtStringBacking *manual = (XrtStringBacking *)pending;
    atomic_init(&manual->cached_hash, 0);
    manual->length = manual->runes = 1; manual->bytes[0] = 'z'; manual->bytes[1] = 0;
    BAD(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "other", 5, 0));
    OK(xrt_managed_slot_commit(fixture.frame, 0));
    CHECK(borrow(&fixture, 0) == pending);
    fixture_done(&fixture);
}
static void scalar_arrays(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    const int64_t signed_values[] = {INT64_MIN, -1, INT64_MAX};
    const uint8_t bytes[] = {0, 255, 3};
    const uint64_t words[] = {UINT64_MAX, UINT64_C(1) << 63, 0};
    OK(xrt_array_backing_i64_create(fixture.frame, 0, &fixture.types, signed_values, 3, 5));
    OK(xrt_array_backing_u8_create(fixture.frame, 1, &fixture.types, bytes, 3, 8));
    OK(xrt_array_backing_u64_create(fixture.frame, 2, &fixture.types, words, 3, 3));
    const XrtBackingArrayKind kinds[] = {XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64};
    for (size_t slot = 0; slot < 3; ++slot) {
        const XrtArrayBacking *array = (const XrtArrayBacking *)borrow(&fixture, slot);
        CHECK(array->length == 3 && array->managed.ops == fixture.types.arrays[kinds[slot]]);
        for (size_t i = 0; i < 3; ++i) {
            uint64_t actual = 99, expected = 0;
            OK(xrt_array_backing_read_bits(array, kinds[slot], i, &actual));
            if (!slot) memcpy(&expected, &signed_values[i], sizeof(expected));
            else expected = slot == 1 ? bytes[i] : words[i];
            CHECK(actual == expected);
        }
        uint64_t unchanged = 17;
        BAD(xrt_array_backing_read_bits(array, kinds[slot], 3, &unchanged));
        CHECK(unchanged == 17);
    }
    const XrtArrayBacking *source = (const XrtArrayBacking *)borrow(&fixture, 2);
    OK(xrt_array_backing_copy_create(fixture.frame, 3, &fixture.types, XRT_BACKING_ARRAY_U64, source, 7));
    const XrtArrayBacking *copy = (const XrtArrayBacking *)borrow(&fixture, 3);
    CHECK(copy != source && copy->capacity == 7 && copy->length == 3 && !memcmp(copy->elements, words, sizeof(words)));
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_array_backing_i64_create(fixture.frame, 0, &fixture.types, signed_values, 3, 2));
    CHECK(xrt_array_backing_u64_create(fixture.frame, 0, &fixture.types, NULL, 0, SIZE_MAX) == XRT_MANAGED_OVERFLOW);
    same_bill(fixture.billing, baseline);
    fixture_done(&fixture);
}
static void string_arrays(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "hello", 5, 0));
    XrtStringBacking *string = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *values[] = {string, string, string};
    OK(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, values, 3, 7));
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&fixture, 1);
    CHECK(array->capacity == 7 && array->length == 3 && xr_signed_rc_read(&string->managed.references) == 4);
    for (size_t i = 0; i < array->length; ++i) CHECK(tail(array, i) == &string->managed);
    const XrtStringBacking *borrowed = NULL;
    OK(xrt_array_backing_string_at(array, 2, &borrowed));
    CHECK(borrowed == string);
    BAD(xrt_array_backing_string_at(array, 0, &borrowed));
    borrowed = NULL;
    BAD(xrt_array_backing_string_at(array, 3, &borrowed));
    CHECK(!borrowed);
    OK(xrt_array_backing_copy_create(fixture.frame, 2, &fixture.types, XRT_BACKING_ARRAY_STRING, array, 9));
    CHECK(xr_signed_rc_read(&string->managed.references) == 7);
    OK(xrt_backing_slot_copy(fixture.frame, 3, &array->managed));
    OK(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, NULL, 0, 4));
    CHECK(borrow(&fixture, 3) == &array->managed && array->length == 3);
    XrtArrayBacking *empty = (XrtArrayBacking *)borrow(&fixture, 1);
    CHECK(empty->length == 0 && empty->capacity == 4);
    XrtStringBacking *invalid[] = {string, NULL};
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, invalid, 2, 4));
    CHECK(borrow(&fixture, 1) == &empty->managed);
    same_bill(fixture.billing, baseline);
    fixture_done(&fixture);
}
static void array_reserve_preserves_snapshots(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "a", 1, 0));
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, "b", 1, 0));
    XrtStringBacking *first = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *second = (XrtStringBacking *)borrow(&fixture, 1);
    XrtStringBacking *values[] = {first, second};
    OK(xrt_array_backing_string_create(fixture.frame, 2, &fixture.types, values, 2, 4));
    const XrtManagedHeader *source = borrow(&fixture, 2);
    int64_t capacity = -1;
    BAD(xrt_array_backing_capacity(source, XRT_BACKING_ARRAY_I64, &capacity));
    CHECK(capacity == -1);
    OK(xrt_array_backing_capacity(source, XRT_BACKING_ARRAY_STRING, &capacity));
    CHECK(capacity == 4);
    OK(xrt_array_backing_reserve_create(fixture.frame, 3, &fixture.types, XRT_BACKING_ARRAY_STRING, source, 1));
    CHECK(borrow(&fixture, 3) == source);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_array_backing_reserve_create(fixture.frame, 4, &fixture.types, XRT_BACKING_ARRAY_I64, source, 8));
    CHECK(xrt_array_backing_reserve_create(fixture.frame, 4, &fixture.types, XRT_BACKING_ARRAY_STRING,
        source, INT64_MAX) == XRT_MANAGED_OVERFLOW);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_array_backing_reserve_create(fixture.frame, 4, &fixture.types, XRT_BACKING_ARRAY_STRING,
        source, 8) == XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    atomic_store(&second->managed.references, INT32_MAX);
    CHECK(xrt_array_backing_reserve_create(fixture.frame, 4, &fixture.types, XRT_BACKING_ARRAY_STRING,
        source, 8) == XRT_MANAGED_REFCOUNT);
    atomic_store(&second->managed.references, 2);
    CHECK(!borrow(&fixture, 4) && borrow(&fixture, 2) == source && xr_signed_rc_read(&first->managed.references) == 2);
    same_bill(fixture.billing, baseline);
    fixture.audit.allocation_fill = 0xa5;
    OK(xrt_array_backing_reserve_create(fixture.frame, 4, &fixture.types, XRT_BACKING_ARRAY_STRING, source, 8));
    XrtArrayBacking *grown = (XrtArrayBacking *)borrow(&fixture, 4);
    CHECK(&grown->managed != source && grown->capacity == 8 && grown->length == 2);
    CHECK(tail(grown, 0) == &first->managed && tail(grown, 1) == &second->managed);
    CHECK(grown->elements[7 * sizeof(XrtManagedHeader *)] == 0xa5);
    XrtArrayUpdate append = {.operation=XRT_ARRAY_UPDATE_PUSH,
        .item={.kind=XRT_BACKING_ARRAY_STRING,.value.string=second}};
    bool applied = false;
    OK(xrt_array_backing_try_update(grown,&append,&applied));
    CHECK(applied && grown->length == 3 && tail(grown, 2) == &second->managed);
    OK(xrt_array_backing_capacity(source, XRT_BACKING_ARRAY_STRING, &capacity));
    CHECK(capacity == 4 && xr_signed_rc_read(&first->managed.references) == 3);
    fixture_done(&fixture);
}
static void construction_failures(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "a", 1, 0));
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, "b", 1, 0));
    OK(xrt_array_backing_string_create(fixture.frame, 2, &fixture.types, NULL, 0, 2));
    XrtStringBacking *first = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *second = (XrtStringBacking *)borrow(&fixture, 1);
    XrtManagedHeader *old = borrow(&fixture, 2);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "new", 3, 0) == XRT_MANAGED_OOM);
    XrtStringBacking *values[] = {first, second, first};
    CHECK(xrt_array_backing_string_create(fixture.frame, 2, &fixture.types, values, 3, 8) == XRT_MANAGED_OOM);
    CHECK(borrow(&fixture, 0) == &first->managed && borrow(&fixture, 2) == old);
    same_bill(fixture.billing, baseline);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    CHECK(xrt_array_backing_u64_create(fixture.frame, 2, &fixture.types, NULL, 0, 4096) == XRT_MANAGED_LIMIT);
    atomic_store(&second->managed.references, INT32_MAX);
    CHECK(xrt_array_backing_string_create(fixture.frame, 2, &fixture.types, values, 3, 8) == XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&first->managed.references) == 1 && borrow(&fixture, 2) == old);
    atomic_store(&second->managed.references, 1);
    same_bill(fixture.billing, baseline);
    xrt_billing_owner_close(fixture.billing);
    CHECK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "new", 3, 0) == XRT_MANAGED_CLOSED);
    CHECK(borrow(&fixture, 0) == &first->managed);
    OK(xrt_backing_slot_copy(fixture.frame, 3, &first->managed));
    same_bill(fixture.billing, baseline);
    fixture_done(&fixture);
}
static void pending_capacity(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "owned", 5, 0));
    OK(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, NULL, 0, 1));
    XrtManagedHeader *old = borrow(&fixture, 1), *string = borrow(&fixture, 0), *payload = NULL;
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    fixture.audit.allocation_fill = 0xa5;
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.types.arrays[XRT_BACKING_ARRAY_STRING],
        sizeof(XrtArrayBacking) + 5 * sizeof(string), &payload));
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    CHECK(array->length == 0 && array->capacity == 5 && array->elements[0] == 0xa5);
    CHECK(xrt_managed_retain(string));
    memcpy(array->elements, &string, sizeof(string));
    array->length = 1;
    array->capacity = SIZE_MAX;
    BAD(xrt_managed_slot_commit(fixture.frame, 1));
    OK(xrt_managed_slot_abort(fixture.frame, 1));
    CHECK(borrow(&fixture, 1) == old && xr_signed_rc_read(&string->references) == 1);
    same_bill(fixture.billing, baseline);
    fixture_done(&fixture);
}
static void jump_pending(Fixture *fixture) {
    jmp_buf target;
    if (!setjmp(target)) {
        XrtManagedHeader *payload = NULL, *string = borrow(fixture, 0);
        fixture->audit.allocation_fill = 0xa5;
        OK(xrt_managed_slot_prepare(fixture->frame, 1, fixture->types.arrays[XRT_BACKING_ARRAY_STRING],
            sizeof(XrtArrayBacking) + 4 * sizeof(string), &payload));
        CHECK(xrt_managed_retain(string));
        XrtArrayBacking *array = (XrtArrayBacking *)payload;
        memcpy(array->elements, &string, sizeof(string));
        array->length = 1;
        longjmp(target, 1);
    }
    OK(xrt_managed_root_drain(fixture->root));
}
static void nonlocal_retirement(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "kept", 4, 0));
    jump_pending(&fixture);
    fixture.frame = NULL;
    fixture_done(&fixture);
}
static void independent_lifetimes(void) {
    Fixture strings, arrays;
    fixture_init(&strings, SIZE_MAX);
    fixture_init(&arrays, SIZE_MAX);
    OK(xrt_string_backing_create(strings.frame, 0, &strings.types, "cross", 5, 0));
    XrtManagedHeader *owned_string = NULL;
    OK(xrt_managed_slot_take(strings.frame, 0, &owned_string));
    XrtStringBacking *values[] = {(XrtStringBacking *)owned_string, (XrtStringBacking *)owned_string};
    OK(xrt_array_backing_string_create(arrays.frame, 0, &arrays.types, values, 2, 8));
    xrt_managed_drop(owned_string);
    XrtManagedHeader *owned_array = NULL;
    OK(xrt_managed_slot_take(arrays.frame, 0, &owned_array));
    fixture_retire(&strings);
    fixture_retire(&arrays);
    CHECK(!atomic_load(&strings.audit.finalized) && !atomic_load(&arrays.audit.finalized));
    const XrtStringBacking *borrowed = NULL;
    OK(xrt_array_backing_string_at((const XrtArrayBacking *)owned_array, 1, &borrowed));
    CHECK(borrowed->length == 5 && !memcmp(borrowed->bytes, "cross", 6));
    CHECK(xr_signed_rc_read(&borrowed->managed.references) == 2);
    xrt_managed_drop(owned_array);
    CHECK(atomic_load(&strings.audit.finalized) == 1 && atomic_load(&arrays.audit.finalized) == 1);
    CHECK(!atomic_load(&strings.audit.bytes) && !atomic_load(&arrays.audit.bytes));
}
static XrtArrayItem scalar_item(XrtBackingArrayKind kind, uint64_t bits) {
    XrtArrayItem item = {.kind=kind};
    if (kind == XRT_BACKING_ARRAY_I64) memcpy(&item.value.i64, &bits, sizeof(bits));
    else if (kind == XRT_BACKING_ARRAY_U8) item.value.u8 = (uint8_t)bits;
    else item.value.u64 = bits;
    return item;
}
static uint64_t scalar_at(const XrtArrayBacking *array, XrtBackingArrayKind kind, size_t index) {
    uint64_t value = 0;
    OK(xrt_array_backing_read_bits(array, kind, index, &value));
    return value;
}
static void scalar_updates(void) {
    const XrtBackingArrayKind kinds[] = {XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64};
    for (size_t family = 0; family < 3; ++family) {
        Fixture fixture;
        fixture_init(&fixture, SIZE_MAX);
        XrtBackingArrayKind kind = kinds[family];
        if (kind == XRT_BACKING_ARRAY_I64) OK(xrt_array_backing_i64_create(fixture.frame, 0, &fixture.types, NULL, 0, 2));
        else if (kind == XRT_BACKING_ARRAY_U8) OK(xrt_array_backing_u8_create(fixture.frame, 0, &fixture.types, NULL, 0, 2));
        else OK(xrt_array_backing_u64_create(fixture.frame, 0, &fixture.types, NULL, 0, 2));
        XrtManagedHeader *owned = NULL;
        OK(xrt_managed_slot_take(fixture.frame, 0, &owned));
        XrtArrayBacking *binding = (XrtArrayBacking *)owned;
        XrtArrayUpdate edit = {XRT_ARRAY_UPDATE_PUSH, 0, scalar_item(kind, 17)};
        bool applied = false;
        XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
        xrt_billing_owner_fail_after(fixture.billing, 0);
        OK(xrt_array_backing_try_update(binding, &edit, &applied));
        CHECK(applied && binding->length == 1 && scalar_at(binding, kind, 0) == 17);
        edit.item = scalar_item(kind, 23);
        OK(xrt_array_backing_try_update(binding, &edit, &applied));
        CHECK(applied && binding->length == 2);
        edit.item = scalar_item(kind, 29);
        OK(xrt_array_backing_try_update(binding, &edit, &applied));
        CHECK(!applied && binding->length == 2);
        same_bill(fixture.billing, baseline);
        xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
        OK(xrt_managed_slot_adopt(fixture.frame, 0, &owned));
        OK(xrt_backing_slot_copy(fixture.frame, 1, &binding->managed));
        edit.operation = XRT_ARRAY_UPDATE_SET; edit.index = 0;
        OK(xrt_array_backing_try_update(binding, &edit, &applied));
        CHECK(!applied && scalar_at(binding, kind, 0) == 17);
        OK(xrt_array_backing_update_create(fixture.frame, 0, &fixture.types, binding, &edit));
        XrtArrayBacking *next = (XrtArrayBacking *)borrow(&fixture, 0);
        CHECK(next != binding && next->length == 2 && scalar_at(next, kind, 0) == 29);
        CHECK(scalar_at(next, kind, 1) == 23);
        CHECK(scalar_at(binding, kind, 0) == 17 && scalar_at(binding, kind, 1) == 23);
        edit.operation = XRT_ARRAY_UPDATE_PUSH; edit.item = scalar_item(kind, UINT64_MAX);
        OK(xrt_array_backing_update_create(fixture.frame, 0, &fixture.types, next, &edit));
        next = (XrtArrayBacking *)borrow(&fixture, 0);
        CHECK(next->length == 3 && next->capacity == 4);
        CHECK(scalar_at(next, kind, 0) == 29 && scalar_at(next, kind, 1) == 23);
        CHECK(scalar_at(next, kind, 2) == (kind == XRT_BACKING_ARRAY_U8 ? 255 : UINT64_MAX));
        baseline = xrt_billing_owner_stats(fixture.billing);
        edit.operation = XRT_ARRAY_UPDATE_SET; edit.index = -1;
        CHECK(xrt_array_backing_try_update(next, &edit, &applied) == XRT_MANAGED_BOUNDS && !applied);
        CHECK(xrt_array_backing_update_create(fixture.frame, 0, &fixture.types, next, &edit) == XRT_MANAGED_BOUNDS);
        edit.index = 3;
        CHECK(xrt_array_backing_try_update(next, &edit, &applied) == XRT_MANAGED_BOUNDS && !applied);
        CHECK(borrow(&fixture, 0) == &next->managed && next->length == 3);
        same_bill(fixture.billing, baseline);
        atomic_store(&next->managed.references, -1);
        edit.index = 1;
        OK(xrt_array_backing_try_update(next, &edit, &applied));
        CHECK(applied && xr_signed_rc_read(&next->managed.references) == 1);
        size_t saved_capacity = next->capacity;
        next->length = next->capacity = INT64_MAX;
        edit.operation = XRT_ARRAY_UPDATE_PUSH;
        CHECK(xrt_array_backing_try_update(next, &edit, &applied) == XRT_MANAGED_OVERFLOW && !applied);
        CHECK(xrt_array_backing_update_create(fixture.frame, 0, &fixture.types, next, &edit) == XRT_MANAGED_OVERFLOW);
        next->length = 3; next->capacity = saved_capacity;
        fixture_done(&fixture);
    }
}
/* This standalone ownership test supplies a deterministic provider. The
 * production archive links the platform implementation for generated C and VM. */
static size_t entropy_calls, entropy_size;
static uint8_t entropy_byte;
void xr_random_bytes(unsigned char *bytes, size_t size) {
    ++entropy_calls;
    entropy_size = size;
    memset(bytes, entropy_byte, size);
}
static void array_random_updates(void) {
    Fixture caller, callee;
    fixture_init(&caller, SIZE_MAX);
    fixture_init(&callee, SIZE_MAX);
    const uint8_t initial[] = {17, 23, 29};
    const size_t capacity = 8;
    const int64_t other_value = 71;
    OK(xrt_array_backing_u8_create(caller.frame, 0, &caller.types, initial, sizeof(initial), capacity));
    OK(xrt_array_backing_i64_create(caller.frame, 3, &caller.types, &other_value, 1, 1));
    OK(xrt_array_backing_u8_create(caller.frame, 4, &caller.types, NULL, 0, capacity));
    XrtArrayBacking *original = (XrtArrayBacking *)borrow(&caller, 0);
    XrtArrayBacking *empty = (XrtArrayBacking *)borrow(&caller, 4);
    memset(original->elements + sizeof(initial), 0xa5, capacity - sizeof(initial));
    OK(xrt_backing_slot_copy(caller.frame, 5, &empty->managed));
    const XrtPlace binding = {.frame=caller.frame, .slot=0, .nulls=XRT_PLACE_NONNULL};
    const XrtArrayUpdate fill = {.operation=XRT_ARRAY_UPDATE_RANDOM_BYTES, .item={.kind=XRT_BACKING_ARRAY_U8}};
    XrtBillingStats caller_before = xrt_billing_owner_stats(caller.billing);
    XrtBillingStats callee_before = xrt_billing_owner_stats(callee.billing);
    xrt_billing_owner_fail_after(callee.billing, 0);

    entropy_byte = 0xd3;
    OK(xrt_array_update_place(binding, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &fill));
    CHECK(entropy_calls == 1 && entropy_size == sizeof(initial));
    CHECK(borrow(&caller, 0) == &original->managed && !borrow(&callee, 1));
    CHECK(original->length == sizeof(initial) && original->capacity == capacity);
    const uint8_t filled[] = {0xd3, 0xd3, 0xd3, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
    CHECK(!memcmp(original->elements, filled, capacity));
    same_bill(caller.billing, caller_before);
    same_bill(callee.billing, callee_before);
    CHECK(xrt_billing_owner_stats(callee.billing).allocations == callee_before.allocations);

    OK(xrt_backing_slot_copy(caller.frame, 2, &original->managed));
    CHECK(xrt_array_update_place(binding, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &fill)
        == XRT_MANAGED_OOM);
    CHECK(entropy_calls == 1 && borrow(&caller, 0) == &original->managed &&
        borrow(&caller, 2) == &original->managed && !borrow(&callee, 1));
    CHECK(!memcmp(original->elements, filled, capacity));
    same_bill(caller.billing, caller_before);
    same_bill(callee.billing, callee_before);

    XrtPlace other = binding;
    other.slot = 3;
    BAD(xrt_array_update_place(other, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &fill));
    XrtArrayUpdate wrong = fill;
    wrong.item.kind = XRT_BACKING_ARRAY_I64;
    BAD(xrt_array_update_place(other, XRT_BACKING_ARRAY_I64, callee.frame, 1, &callee.types, &wrong));
    CHECK(scalar_at((XrtArrayBacking *)borrow(&caller, 3), XRT_BACKING_ARRAY_I64, 0) == 71);
    wrong = fill;
    wrong.index = 1;
    BAD(xrt_array_update_place(binding, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &wrong));
    other.slot = 4;
    OK(xrt_array_update_place(other, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &fill));
    CHECK(entropy_calls == 1 && borrow(&caller, 4) == &empty->managed && !borrow(&callee, 1));
    CHECK(!empty->length && empty->capacity == capacity);

    xrt_billing_owner_fail_after(callee.billing, SIZE_MAX);
    callee.audit.allocation_fill = 0xa5;
    entropy_byte = 0x4c;
    OK(xrt_array_update_place(binding, XRT_BACKING_ARRAY_U8, callee.frame, 1, &callee.types, &fill));
    XrtArrayBacking *replacement = (XrtArrayBacking *)borrow(&caller, 0);
    CHECK(replacement && replacement != original && replacement->length == sizeof(initial) &&
        replacement->capacity == capacity && !borrow(&callee, 1));
    CHECK(entropy_calls == 2 && entropy_size == sizeof(initial));
    const uint8_t refilled[] = {0x4c, 0x4c, 0x4c, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
    CHECK(!memcmp(replacement->elements, refilled, capacity));
    CHECK(borrow(&caller, 2) == &original->managed && !memcmp(original->elements, filled, capacity));
    CHECK(xrt_billing_owner_stats(callee.billing).allocations == callee_before.allocations + 1);
    OK(xrt_managed_slot_drop(caller.frame, 0));
    same_bill(callee.billing, callee_before);
    fixture_done(&callee);
    fixture_done(&caller);
}
static void local_i64_growth(void) {
    size_t full_bytes = 0;
    const size_t old_extent = sizeof(XrtArrayBacking) + 4 * sizeof(int64_t);
    const size_t new_extent = sizeof(XrtArrayBacking) + 8 * sizeof(int64_t);
    const int64_t initial[] = {11, 13, 17, 19};
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture fixture = {0};
        OK(xrt_billing_owner_new(NULL, mode == 2 ? full_bytes + new_extent - old_extent : SIZE_MAX,
            &fixture.billing));
        XrtBillingStats empty = xrt_billing_owner_stats(fixture.billing);
        OK(xrt_backing_types_init(fixture.billing, &fixture.types));
        OK(xrt_managed_root_new(fixture.billing, &fixture.root));
        OK(xrt_managed_frame_enter(fixture.root, 2, &fixture.frame));
        OK(xrt_array_backing_i64_create(fixture.frame, 0, &fixture.types, initial, 4, 4));
        XrtPlace place = {.frame=fixture.frame, .slot=0, .nulls=XRT_PLACE_NONNULL};
        XrtManagedHeader *snapshot = mode == 1 ? borrow(&fixture, 0) : NULL;
        if (snapshot) CHECK(xrt_managed_retain(snapshot));
        XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
        if (!mode) {
            full_bytes = before.bytes;
            XrtManagedHeader *old = borrow(&fixture, 0);
            xrt_billing_owner_fail_after(fixture.billing, 0);
            CHECK(xrt_array_push_local_i64(place, 1, &fixture.types, 23) == XRT_MANAGED_OOM);
            CHECK(borrow(&fixture, 0) == old && ((XrtArrayBacking *)old)->length == 4);
            CHECK(!memcmp(((XrtArrayBacking *)old)->elements, initial, sizeof(initial)));
            same_bill(fixture.billing, before);
            CHECK(xrt_billing_owner_stats(fixture.billing).allocations == before.allocations);
            xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
        }
        XrtManagedStatus status = xrt_array_push_local_i64(place, 1, &fixture.types, INT64_MIN);
        CHECK(status == (mode == 2 ? XRT_MANAGED_LIMIT : XRT_MANAGED_OK));
        XrtArrayBacking *array = (XrtArrayBacking *)borrow(&fixture, 0);
        CHECK(!memcmp(array->elements, initial, sizeof(initial)));
        if (mode == 2) {
            CHECK(array->length == 4 && array->capacity == 4);
            same_bill(fixture.billing, before);
        } else {
            CHECK(array->length == 5 && array->capacity == 8 &&
                scalar_at(array, XRT_BACKING_ARRAY_I64, 4) == (uint64_t)INT64_MIN);
            if (snapshot) {
                XrtArrayBacking *old = (XrtArrayBacking *)snapshot;
                CHECK(old != array && old->length == 4 && old->capacity == 4);
                CHECK(!memcmp(old->elements, initial, sizeof(initial)));
                xrt_managed_drop(snapshot);
            }
            XrtBillingStats final = xrt_billing_owner_stats(fixture.billing);
            CHECK(final.blocks == before.blocks && final.bytes == before.bytes + new_extent - old_extent &&
                final.allocations == before.allocations + 1);
        }
        XrtManagedSlotLocation workspace = {0};
        OK(xrt_managed_slot_location(fixture.frame, 1, &workspace));
        CHECK(!*workspace.owner && !workspace.pending->value && !workspace.pending->allocation_bytes && !*workspace.busy);
        OK(xrt_managed_root_destroy(&fixture.root));
        xrt_backing_types_drop(&fixture.types);
        same_bill(fixture.billing, empty);
        xrt_billing_owner_drop(fixture.billing);
    }
    Fixture custom;
    fixture_init(&custom, SIZE_MAX);
    OK(xrt_array_backing_i64_create(custom.frame, 0, &custom.types, initial, 4, 4));
    XrtBillingStats before = xrt_billing_owner_stats(custom.billing);
    XrtPlace place = {.frame=custom.frame, .slot=0, .nulls=XRT_PLACE_NONNULL};
    OK(xrt_array_push_local_i64(place, 1, &custom.types, 23));
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&custom, 0);
    CHECK(array->length == 5 && array->capacity == 8 && !memcmp(array->elements, initial, sizeof(initial)));
    CHECK(scalar_at(array, XRT_BACKING_ARRAY_I64, 4) == 23);
    CHECK(atomic_load(&custom.audit.bytes) == before.bytes + new_extent - old_extent &&
        atomic_load(&custom.audit.blocks) == before.blocks);
    fixture_done(&custom);
}
static void string_updates(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "old", 3, 0));
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, "new", 3, 0));
    XrtStringBacking *old = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *fresh = (XrtStringBacking *)borrow(&fixture, 1);
    XrtStringBacking *values[] = {old, old};
    OK(xrt_array_backing_string_create(fixture.frame, 2, &fixture.types, values, 2, 4));
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&fixture, 2);
    XrtArrayUpdate edit = {.operation=XRT_ARRAY_UPDATE_SET, .index=0,
        .item={.kind=XRT_BACKING_ARRAY_STRING, .value.string=old}};
    bool applied = false;
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(applied && tail(array, 0) == &old->managed && xr_signed_rc_read(&old->managed.references) == 3);
    edit.item.value.string = fresh;
    atomic_store(&fresh->managed.references, INT32_MAX);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_array_backing_try_update(array, &edit, &applied) == XRT_MANAGED_REFCOUNT && !applied);
    CHECK(tail(array, 0) == &old->managed && array->length == 2);
    atomic_store(&fresh->managed.references, 1);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(applied && tail(array, 0) == &fresh->managed && xr_signed_rc_read(&old->managed.references) == 2);
    CHECK(xr_signed_rc_read(&fresh->managed.references) == 2);
    same_bill(fixture.billing, baseline);
    OK(xrt_backing_slot_copy(fixture.frame, 3, &array->managed));
    edit.operation = XRT_ARRAY_UPDATE_PUSH; edit.item.value.string = old;
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(!applied && array->length == 2);
    CHECK(xrt_array_backing_update_create(fixture.frame, 2, &fixture.types, array, &edit) == XRT_MANAGED_OOM);
    CHECK(borrow(&fixture, 2) == &array->managed && tail(array, 0) == &fresh->managed);
    same_bill(fixture.billing, baseline);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    atomic_store(&old->managed.references, INT32_MAX);
    CHECK(xrt_array_backing_update_create(fixture.frame, 2, &fixture.types, array, &edit) == XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&fresh->managed.references) == 2 && borrow(&fixture, 2) == &array->managed);
    same_bill(fixture.billing, baseline);
    atomic_store(&old->managed.references, 2);
    OK(xrt_array_backing_update_create(fixture.frame, 2, &fixture.types, array, &edit));
    XrtArrayBacking *next = (XrtArrayBacking *)borrow(&fixture, 2);
    CHECK(next != array && next->length == 3 && array->length == 2);
    CHECK(tail(next, 0) == &fresh->managed && tail(next, 1) == &old->managed && tail(next, 2) == &old->managed);
    // Replacing the sole old element does not retain a child that is discarded.
    edit.operation = XRT_ARRAY_UPDATE_SET; edit.index = 0; edit.item.value.string = old;
    atomic_store(&fresh->managed.references, INT32_MAX);
    OK(xrt_array_backing_update_create(fixture.frame, 2, &fixture.types, next, &edit));
    atomic_store(&fresh->managed.references, 2);
    CHECK(tail((XrtArrayBacking *)borrow(&fixture, 2), 0) == &old->managed);
    fixture_done(&fixture);
}
static void sole_child_updates(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "sole", 4, 0));
    XrtStringBacking *child = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *values[] = {child};
    OK(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, values, 1, 1));
    OK(xrt_managed_slot_drop(fixture.frame, 0));
    CHECK(xr_signed_rc_read(&child->managed.references) == 1);
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&fixture, 1);
    XrtArrayItem rhs = {0};
    OK(xrt_array_backing_get(array, XRT_BACKING_ARRAY_STRING, 0, &rhs));
    XrtArrayUpdate edit = {XRT_ARRAY_UPDATE_PUSH, 0, rhs};
    bool applied = false;
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(!applied && xr_signed_rc_read(&child->managed.references) == 1);
    OK(xrt_array_backing_update_create(fixture.frame, 1, &fixture.types, array, &edit));
    array = (XrtArrayBacking *)borrow(&fixture, 1);
    CHECK(array->length == 2 && array->capacity == 4 && xr_signed_rc_read(&child->managed.references) == 2);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(applied && array->length == 3 && xr_signed_rc_read(&child->managed.references) == 3);
    edit.operation = XRT_ARRAY_UPDATE_SET; edit.index = 0;
    OK(xrt_array_backing_try_update(array, &edit, &applied));
    CHECK(applied && xr_signed_rc_read(&child->managed.references) == 3);
    same_bill(fixture.billing, baseline);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.types.string, sizeof(XrtStringBacking) + 2, &pending));
    ((XrtStringBacking *)pending)->bytes[0] = 0;
    BAD(xrt_array_backing_update_create(fixture.frame, 1, &fixture.types, array, &edit));
    CHECK(borrow(&fixture, 1) == &array->managed);
    OK(xrt_managed_slot_abort(fixture.frame, 1));
    fixture_done(&fixture);
}
static void repeat_and_get(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    for (unsigned family = 0; family < XRT_BACKING_ARRAY_COUNT; ++family) {
        XrtBackingArrayKind kind = (XrtBackingArrayKind)family;
        XrtArrayItem fill = scalar_item(kind, UINT64_MAX);
        if (kind == XRT_BACKING_ARRAY_STRING) {
            OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "\303\251", 2, 0));
            fill.value.string = (XrtStringBacking *)borrow(&fixture, 0);
        }
        OK(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, 5));
        XrtArrayBacking *array = (XrtArrayBacking *)borrow(&fixture, 1);
        CHECK(array->length == 5 && array->capacity == 8);
        for (int64_t i = 0; i < 5; ++i) {
            XrtArrayItem item = {.kind=XRT_BACKING_ARRAY_COUNT};
            OK(xrt_array_backing_get(array, kind, i, &item));
            CHECK(item.kind == kind);
            if (kind == XRT_BACKING_ARRAY_STRING) {
                CHECK(item.value.string == fill.value.string && xr_signed_rc_read(&item.value.string->managed.references) == 6);
            } else if (kind == XRT_BACKING_ARRAY_I64) CHECK(item.value.i64 == -1);
            else if (kind == XRT_BACKING_ARRAY_U8) CHECK(item.value.u8 == UINT8_MAX);
            else CHECK(item.value.u64 == UINT64_MAX);
        }
        XrtArrayItem unchanged = scalar_item(XRT_BACKING_ARRAY_U64, 37);
        CHECK(xrt_array_backing_get(array, kind, -1, &unchanged) == XRT_MANAGED_BOUNDS);
        CHECK(xrt_array_backing_get(array, kind, 5, &unchanged) == XRT_MANAGED_BOUNDS);
        CHECK(unchanged.kind == XRT_BACKING_ARRAY_U64 && unchanged.value.u64 == 37);
        BAD(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, -1));
        CHECK(borrow(&fixture, 1) == &array->managed && array->length == 5);
        if (kind == XRT_BACKING_ARRAY_STRING) {
            XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
            atomic_store(&fill.value.string->managed.references, INT32_MAX - 2);
            CHECK(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, 3) == XRT_MANAGED_REFCOUNT);
            CHECK(xr_signed_rc_read(&fill.value.string->managed.references) == INT32_MAX - 2);
            atomic_store(&fill.value.string->managed.references, 6);
            CHECK(borrow(&fixture, 1) == &array->managed);
            same_bill(fixture.billing, baseline);
            XrtArrayItem borrowed = {0};
            OK(xrt_array_backing_get(array, kind, 4, &borrowed));
            OK(xrt_backing_slot_copy(fixture.frame, 2, &borrowed.value.string->managed));
        }
        OK(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, 0));
        array = (XrtArrayBacking *)borrow(&fixture, 1);
        CHECK(!array->length && !array->capacity);
        if (kind == XRT_BACKING_ARRAY_STRING) OK(xrt_managed_slot_drop(fixture.frame, 0));
    }
    XrtArrayItem fill = scalar_item(XRT_BACKING_ARRAY_I64, 7);
    CHECK(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, INT64_MAX) == XRT_MANAGED_OVERFLOW);
    fill.kind = XRT_BACKING_ARRAY_U8; fill.value.u8 = 7;
    CHECK(xrt_array_backing_repeat_create(fixture.frame, 1, &fixture.types, &fill, INT64_MAX) == XRT_MANAGED_LIMIT);
    CHECK(((const XrtStringBacking *)borrow(&fixture, 2))->length == 2);
    fixture_done(&fixture);
}
static void array_displays(void) {
    static const char *const expected[] = {
        "[-9223372036854775808, 9223372036854775807]",
        "[\"a\"b\\c\n\", \"\344\275\240\345\245\275\360\237\214\215\"]",
        "[0, 255]", "[9223372036854775808, 18446744073709551615]"
    };
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    for (unsigned family = 0; family < XRT_BACKING_ARRAY_COUNT; ++family) {
        XrtBackingArrayKind kind = (XrtBackingArrayKind)family;
        if (kind == XRT_BACKING_ARRAY_I64) {
            const int64_t values[] = {INT64_MIN, INT64_MAX};
            OK(xrt_array_backing_i64_create(fixture.frame, 0, &fixture.types, values, 2, 2));
        } else if (kind == XRT_BACKING_ARRAY_STRING) {
            OK(xrt_string_backing_create(fixture.frame, 2, &fixture.types, "a\"b\\c\n", 6, 0));
            OK(xrt_string_backing_create(fixture.frame, 3, &fixture.types, "\344\275\240\345\245\275\360\237\214\215", 10, 0));
            XrtStringBacking *values[] = {(XrtStringBacking *)borrow(&fixture, 2), (XrtStringBacking *)borrow(&fixture, 3)};
            OK(xrt_array_backing_string_create(fixture.frame, 0, &fixture.types, values, 2, 2));
        } else if (kind == XRT_BACKING_ARRAY_U8) {
            const uint8_t values[] = {0, UINT8_MAX};
            OK(xrt_array_backing_u8_create(fixture.frame, 0, &fixture.types, values, 2, 2));
        } else {
            const uint64_t values[] = {UINT64_C(1) << 63, UINT64_MAX};
            OK(xrt_array_backing_u64_create(fixture.frame, 0, &fixture.types, values, 2, 2));
        }
        const XrtArrayBacking *array = (const XrtArrayBacking *)borrow(&fixture, 0);
        OK(xrt_array_backing_display_create(fixture.frame, 1, &fixture.types, array, kind));
        const XrtStringBacking *text = (const XrtStringBacking *)borrow(&fixture, 1);
        size_t length = strlen(expected[family]);
        CHECK(text->length == length && text->capacity == length + 1 && !memcmp(text->bytes, expected[family], length + 1));
        CHECK(text->runes == length - (kind == XRT_BACKING_ARRAY_STRING ? 7 : 0));
        XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
        xrt_billing_owner_fail_after(fixture.billing, 0);
        CHECK(xrt_array_backing_display_create(fixture.frame, 1, &fixture.types, array, kind) == XRT_MANAGED_OOM);
        CHECK(borrow(&fixture, 1) == &text->managed && borrow(&fixture, 0) == &array->managed);
        same_bill(fixture.billing, baseline);
        xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
        // The source stays alive while its very slot is replaced by display text.
        OK(xrt_array_backing_display_create(fixture.frame, 0, &fixture.types, array, kind));
        text = (const XrtStringBacking *)borrow(&fixture, 0);
        CHECK(text->length == length && !memcmp(text->bytes, expected[family], length + 1));
    }
    XrtArrayItem fill = scalar_item(XRT_BACKING_ARRAY_I64, 7);
    OK(xrt_array_backing_repeat_create(fixture.frame, 0, &fixture.types, &fill, 35));
    OK(xrt_array_backing_display_create(fixture.frame, 1, &fixture.types,
        (const XrtArrayBacking *)borrow(&fixture, 0), fill.kind));
    const XrtStringBacking *text = (const XrtStringBacking *)borrow(&fixture, 1);
    CHECK(text->length == 109 && !memcmp(text->bytes + 95, ", ...(3 more)]", 14));
    OK(xrt_array_backing_repeat_create(fixture.frame, 0, &fixture.types, &fill, 0));
    OK(xrt_array_backing_display_create(fixture.frame, 1, &fixture.types,
        (const XrtArrayBacking *)borrow(&fixture, 0), fill.kind));
    text = (const XrtStringBacking *)borrow(&fixture, 1);
    CHECK(text->length == 2 && text->runes == 2 && !memcmp(text->bytes, "[]", 3));
    OK(xrt_string_backing_create(fixture.frame, 2, &fixture.types, "a\0b", 3, 0));
    fill = (XrtArrayItem){.kind=XRT_BACKING_ARRAY_STRING, .value.string=(XrtStringBacking *)borrow(&fixture, 2)};
    OK(xrt_array_backing_repeat_create(fixture.frame, 0, &fixture.types, &fill, 1));
    OK(xrt_array_backing_display_create(fixture.frame, 1, &fixture.types,
        (const XrtArrayBacking *)borrow(&fixture, 0), fill.kind));
    text = (const XrtStringBacking *)borrow(&fixture, 1);
    CHECK(text->length == 7 && text->runes == 7 && !memcmp(text->bytes, "[\"a\0b\"]", 8));
    fixture_done(&fixture);
}
static XrtStringBacking *map_key(Fixture *fixture, size_t slot, const char *bytes, size_t length) {
    OK(xrt_string_backing_create(fixture->frame, slot, &fixture->types, bytes, length, 0));
    return (XrtStringBacking *)borrow(fixture, slot);
}
static void map_value(const XrtMapBacking *map, const XrtStringBacking *key, bool expected_found, int64_t expected) {
    int64_t value = 99; bool found = !expected_found;
    OK(xrt_map_backing_get(map, key, &value, &found));
    CHECK(found == expected_found && value == expected);
}
static void map_check(const XrtMapBacking *map, XrtStringBacking *const *ordered, size_t count) {
    CHECK(map->length == count && count <= map->capacity / 2 && map->order_count <= map->capacity);
    size_t buckets = 0, order = 0;
    for (size_t i = 0; i < map->capacity; ++i) {
        if (i < count) {
            CHECK(map->rows[i].key && map->rows[i].hash == xr_hash_bytes64(map->rows[i].key->bytes, map->rows[i].key->length));
            map_value(map, map->rows[i].key, true, map->rows[i].value);
        } else CHECK(!map->rows[i].key);
        if (map->rows[i].bucket) { CHECK(map->rows[i].bucket <= count); ++buckets; }
        if (map->rows[i].order) {
            CHECK(i < map->order_count && map->rows[i].order <= count && order < count);
            size_t entry = map->rows[i].order - 1;
            CHECK(map->rows[entry].entry_order == i);
            if (ordered) CHECK(map->rows[entry].key == ordered[order]);
            ++order;
        }
        size_t rank = 0;
        for (size_t p = i & (i + 1); p <= i; ++p) rank += map->rows[p].order != 0;
        CHECK(map->rows[i].rank == rank);
    }
    CHECK(buckets == count && order == count);
}
static void map_construction_and_updates(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    XrtStringBacking *first = map_key(&fixture, 0, "same", 4);
    XrtStringBacking *other = map_key(&fixture, 1, "other", 5);
    XrtStringBacking *equal = map_key(&fixture, 2, "same", 4);
    XrtStringBacking *missing = map_key(&fixture, 3, "missing", 7);
    XrtStringBacking *keys[] = {first, other, equal};
    const int64_t values[] = {1, 7, 42};
    atomic_store(&equal->managed.references, INT32_MAX);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    OK(xrt_map_backing_create(fixture.frame, 4, &fixture.types, keys, values, 3));
    atomic_store(&equal->managed.references, 1);
    XrtMapBacking *map = (XrtMapBacking *)borrow(&fixture, 4);
    CHECK(map->capacity == 8 && map->order_count == 2);
    CHECK(xrt_billing_owner_stats(fixture.billing).bytes == before.bytes + 40 + 8 * 56);
    XrtStringBacking *ordered[] = {first, other};
    map_check(map, ordered, 2);
    CHECK(!atomic_load(&missing->cached_hash));
    map_value(map, equal, true, 42); map_value(map, missing, false, 0);
    CHECK(atomic_load(&first->cached_hash) == xr_hash_bytes64("same", 4));
    CHECK(first != equal && atomic_load(&equal->cached_hash) == atomic_load(&first->cached_hash));
    CHECK(atomic_load(&missing->cached_hash) == xr_hash_bytes64("missing", 7));
    CHECK(xr_signed_rc_read(&first->managed.references) == 2 && xr_signed_rc_read(&equal->managed.references) == 1);
    int64_t unchanged = 73; bool found = true;
    BAD(xrt_map_backing_get(map, NULL, &unchanged, &found));
    CHECK(unchanged == 73 && found);
    before = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_map_backing_create(fixture.frame, 4, &fixture.types, keys, values, SIZE_MAX) == XRT_MANAGED_OVERFLOW);
    XrtStringBacking *invalid[] = {first, NULL};
    BAD(xrt_map_backing_create(fixture.frame, 4, &fixture.types, invalid, values, 2));
    same_bill(fixture.billing, before);
    XrtManagedHeader *owned = NULL;
    OK(xrt_managed_slot_take(fixture.frame, 4, &owned));
    XrtMapUpdate update = {XRT_MAP_UPDATE_SET, equal, 91};
    bool applied = false, removed = true;
    xrt_billing_owner_fail_after(fixture.billing, 0);
    atomic_store(&equal->managed.references, INT32_MAX);
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    CHECK(applied && !removed && map->rows[0].key == first);
    atomic_store(&equal->managed.references, 1);
    map_value(map, first, true, 91);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_SET, missing, -9};
    atomic_store(&missing->managed.references, INT32_MAX);
    CHECK(xrt_map_backing_try_update(map, &update, &applied, &removed) == XRT_MANAGED_REFCOUNT && !applied);
    atomic_store(&missing->managed.references, 1);
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    CHECK(applied && !removed && xr_signed_rc_read(&missing->managed.references) == 2);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_REMOVE, equal, 0};
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    CHECK(applied && removed && xr_signed_rc_read(&first->managed.references) == 1);
    ordered[0] = other; ordered[1] = missing;
    map_check(map, ordered, 2);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_SET, first, 12};
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    XrtStringBacking *reordered[] = {other, missing, first};
    for (size_t i = 0; i < 32; ++i) {
        update.operation = XRT_MAP_UPDATE_REMOVE;
        OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(applied && removed);
        update.operation = XRT_MAP_UPDATE_SET;
        OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(applied && !removed);
        map_check(map, reordered, 3);
    }
    same_bill(fixture.billing, before);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_managed_slot_adopt(fixture.frame, 4, &owned));
    fixture_done(&fixture);
}
static void map_cow_and_failures(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    XrtStringBacking *a = map_key(&fixture, 0, "a", 1), *b = map_key(&fixture, 1, "b", 1);
    XrtStringBacking *equal = map_key(&fixture, 2, "a", 1), *absent = map_key(&fixture, 3, "absent", 6);
    XrtStringBacking *keys[] = {a, b}; const int64_t values[] = {1, 2};
    OK(xrt_map_backing_create(fixture.frame, 4, &fixture.types, keys, values, 2));
    XrtMapBacking *source = (XrtMapBacking *)borrow(&fixture, 4);
    OK(xrt_backing_slot_copy(fixture.frame, 5, &source->managed));
    XrtMapUpdate update = {XRT_MAP_UPDATE_REMOVE, absent, 0};
    bool applied = false, removed = true;
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_map_backing_try_update(source, &update, &applied, &removed));
    CHECK(applied && !removed && xr_signed_rc_read(&source->managed.references) == 2);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_SET, equal, 99};
    OK(xrt_map_backing_try_update(source, &update, &applied, &removed)); CHECK(!applied);
    removed = true;
    CHECK(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, source, &update, &removed) == XRT_MANAGED_OOM);
    CHECK(removed && borrow(&fixture, 4) == &source->managed);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    atomic_store(&b->managed.references, INT32_MAX);
    CHECK(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, source, &update, &removed) == XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&a->managed.references) == 2 && borrow(&fixture, 4) == &source->managed);
    atomic_store(&b->managed.references, 2);
    same_bill(fixture.billing, before);
    atomic_store(&equal->managed.references, INT32_MAX);
    OK(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, source, &update, &removed));
    atomic_store(&equal->managed.references, 1);
    XrtMapBacking *next = (XrtMapBacking *)borrow(&fixture, 4);
    CHECK(next != source && !removed && next->rows[0].key == a);
    map_check(next, keys, 2); map_value(source, a, true, 1); map_value(next, a, true, 99);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_REMOVE, a, 0};
    atomic_store(&a->managed.references, INT32_MAX);
    OK(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, next, &update, &removed));
    atomic_store(&a->managed.references, 2);
    CHECK(removed);
    next = (XrtMapBacking *)borrow(&fixture, 4);
    map_check(next, &b, 1); map_value(source, a, true, 1);
    before = xrt_billing_owner_stats(fixture.billing);
    XrtStringBacking *many[256]; int64_t numbers[256];
    for (size_t i = 0; i < 256; ++i) { many[i] = b; numbers[i] = (int64_t)i; }
    CHECK(xrt_map_backing_create(fixture.frame, 4, &fixture.types, many, numbers, 256) == XRT_MANAGED_LIMIT);
    CHECK(borrow(&fixture, 4) == &next->managed); same_bill(fixture.billing, before);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 4, fixture.types.string, sizeof(XrtStringBacking) + 2, &pending));
    update = (XrtMapUpdate){XRT_MAP_UPDATE_CLEAR, NULL, 0};
    BAD(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, next, &update, &removed));
    CHECK(borrow(&fixture, 4) == &next->managed);
    OK(xrt_managed_slot_abort(fixture.frame, 4));
    atomic_store(&b->managed.references, INT32_MAX);
    OK(xrt_map_backing_update_create(fixture.frame, 4, &fixture.types, next, &update, &removed));
    atomic_store(&b->managed.references, 2);
    next = (XrtMapBacking *)borrow(&fixture, 4);
    CHECK(!next->capacity && !next->length && !removed);
    map_check(source, keys, 2);
    fixture_done(&fixture);
}
static void map_snapshot_ownership(void) {
    Fixture fixture;
    fixture_init(&fixture, 16384);
    XrtStringBacking *a = map_key(&fixture, 0, "a", 1), *b = map_key(&fixture, 1, "b", 1);
    XrtStringBacking *keys[] = {a, b};
    const int64_t values[] = {95, 92};
    OK(xrt_map_backing_create(fixture.frame, 2, &fixture.types, keys, values, 2));
    XrtMapBacking *map = (XrtMapBacking *)borrow(&fixture, 2);
    CHECK(xrt_managed_retain(&map->managed));
    OK(xrt_array_backing_i64_create(fixture.frame, 3, &fixture.types, values, 1, 1));
    XrtManagedHeader *old = borrow(&fixture, 3);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_map_backing_snapshot_held(fixture.frame, 3, &fixture.types, map, true) == XRT_MANAGED_OOM);
    CHECK(xrt_map_backing_display_held(fixture.frame, 3, &fixture.types, map) == XRT_MANAGED_OOM);
    CHECK(borrow(&fixture, 3) == old && xr_signed_rc_read(&map->managed.references) == 2);
    same_bill(fixture.billing, before);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    atomic_store(&b->managed.references, INT32_MAX);
    CHECK(xrt_map_backing_snapshot_held(fixture.frame, 3, &fixture.types, map, true) == XRT_MANAGED_REFCOUNT);
    CHECK(borrow(&fixture, 3) == old && xr_signed_rc_read(&a->managed.references) == 2 &&
        xr_signed_rc_read(&map->managed.references) == 2);
    atomic_store(&b->managed.references, 2);
    same_bill(fixture.billing, before);
    OK(xrt_map_backing_snapshot_held(fixture.frame, 3, &fixture.types, map, true));
    OK(xrt_map_backing_snapshot_held(fixture.frame, 4, &fixture.types, map, false));
    OK(xrt_map_backing_display_held(fixture.frame, 5, &fixture.types, map));
    xrt_managed_drop(&map->managed);
    for (size_t n = 0; n < 3; ++n) OK(xrt_managed_slot_drop(fixture.frame, n));
    const XrtArrayBacking *key_snapshot = (const XrtArrayBacking *)borrow(&fixture, 3);
    const XrtArrayBacking *value_snapshot = (const XrtArrayBacking *)borrow(&fixture, 4);
    const XrtStringBacking *display = (const XrtStringBacking *)borrow(&fixture, 5);
    const char expected[] = "{\"a\": 95, \"b\": 92}";
    CHECK(display->length == sizeof(expected) - 1 && display->runes == display->length &&
        !memcmp(display->bytes, expected, sizeof(expected)));
    CHECK(key_snapshot->length == 2 && value_snapshot->length == 2);
    for (size_t n = 0; n < 2; ++n) {
        const XrtStringBacking *key = NULL;
        uint64_t bits = 0;
        OK(xrt_array_backing_string_at(key_snapshot, n, &key));
        OK(xrt_array_backing_read_bits(value_snapshot, XRT_BACKING_ARRAY_I64, n, &bits));
        CHECK(key == keys[n] && bits == (uint64_t)values[n] && xr_signed_rc_read(&key->managed.references) == 1);
    }
    fixture_done(&fixture);
}
static void map_collisions_and_growth(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    XrtStringBacking *keys[5]; int64_t values[] = {1, 2, 3, 4, 5};
    size_t count = 0;
    for (unsigned candidate = 0; count < 5; ++candidate) {
        char text[24]; int length = snprintf(text, sizeof(text), "key%u", candidate);
        CHECK(length > 0 && (size_t)length < sizeof(text));
        if ((xr_hash_bytes64(text, (size_t)length) & 15) != 15) continue;
        keys[count] = map_key(&fixture, count, text, (size_t)length); ++count;
    }
    OK(xrt_map_backing_create(fixture.frame, 8, &fixture.types, keys, values, 4));
    XrtMapBacking *map = (XrtMapBacking *)borrow(&fixture, 8);
    CHECK(map->capacity == 8);
    XrtMapUpdate update = {XRT_MAP_UPDATE_SET, keys[4], 5};
    bool applied = false, removed = true;
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(!applied);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_map_backing_update_create(fixture.frame, 8, &fixture.types, map, &update, &removed) == XRT_MANAGED_OOM);
    CHECK(borrow(&fixture, 8) == &map->managed && map->length == 4); same_bill(fixture.billing, before);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_map_backing_update_create(fixture.frame, 8, &fixture.types, map, &update, &removed));
    map = (XrtMapBacking *)borrow(&fixture, 8);
    CHECK(map->capacity == 16); map_check(map, keys, 5);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_REMOVE, keys[1], 0};
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(applied && removed);
    XrtStringBacking *ordered[] = {keys[0], keys[2], keys[3], keys[4]};
    map_check(map, ordered, 4); map_value(map, keys[1], false, 0);
    update = (XrtMapUpdate){XRT_MAP_UPDATE_SET, keys[1], -2};
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(applied && !removed);
    XrtStringBacking *reordered[] = {keys[0], keys[2], keys[3], keys[4], keys[1]};
    map_check(map, reordered, 5);
    OK(xrt_backing_slot_copy(fixture.frame, 9, &map->managed));
    update = (XrtMapUpdate){XRT_MAP_UPDATE_CLEAR, NULL, 0};
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed)); CHECK(!applied);
    OK(xrt_map_backing_update_create(fixture.frame, 8, &fixture.types, map, &update, &removed));
    map_check(map, reordered, 5);
    CHECK(!((XrtMapBacking *)borrow(&fixture, 8))->capacity);
    fixture_done(&fixture);
}
static void map_retirement_and_lifetimes(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    XrtStringBacking *key = map_key(&fixture, 0, "a\0\303\251", 4);
    XrtStringBacking *keys[] = {key}; int64_t values[] = {7};
    OK(xrt_map_backing_create(fixture.frame, 1, &fixture.types, keys, values, 1));
    XrtMapBacking *map = (XrtMapBacking *)borrow(&fixture, 1);
    OK(xrt_managed_slot_drop(fixture.frame, 0));
    fixture.audit.watched_map = map; fixture.audit.watched_key = key; fixture.audit.watched_empty = true;
    XrtMapUpdate update = {XRT_MAP_UPDATE_REMOVE, key, 0};
    bool applied = false, removed = false;
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    CHECK(applied && removed && fixture.audit.watched_frees == 1); map_check(map, NULL, 0);
    fixture.audit.watched_key = NULL;
    key = map_key(&fixture, 0, "clear", 5); keys[0] = key;
    OK(xrt_map_backing_create(fixture.frame, 1, &fixture.types, keys, values, 1));
    map = (XrtMapBacking *)borrow(&fixture, 1);
    OK(xrt_managed_slot_drop(fixture.frame, 0));
    fixture.audit.watched_map = map; fixture.audit.watched_key = key;
    update = (XrtMapUpdate){XRT_MAP_UPDATE_CLEAR, NULL, 0};
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_map_backing_try_update(map, &update, &applied, &removed));
    CHECK(applied && !removed && fixture.audit.watched_frees == 2); map_check(map, NULL, 0);
    fixture.audit.watched_key = NULL;
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    key = map_key(&fixture, 0, "pending", 7);
    XrtManagedHeader *payload = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.types.map, 40 + 8 * 56, &payload));
    XrtMapBacking *pending = (XrtMapBacking *)payload;
    CHECK(xrt_managed_retain(&key->managed)); pending->rows[7].key = key;
    pending->capacity = SIZE_MAX;
    BAD(xrt_managed_slot_commit(fixture.frame, 1));
    OK(xrt_managed_slot_abort(fixture.frame, 1));
    CHECK(xr_signed_rc_read(&key->managed.references) == 1 && borrow(&fixture, 1) == &map->managed);
    fixture_done(&fixture);
    Fixture strings, maps;
    fixture_init(&strings, SIZE_MAX); fixture_init(&maps, SIZE_MAX);
    key = map_key(&strings, 0, "cross", 5); keys[0] = key;
    OK(xrt_map_backing_create(maps.frame, 0, &maps.types, keys, values, 1));
    XrtManagedHeader *owned = NULL;
    OK(xrt_managed_slot_take(maps.frame, 0, &owned));
    fixture_retire(&strings); fixture_retire(&maps);
    CHECK(!atomic_load(&strings.audit.finalized) && !atomic_load(&maps.audit.finalized));
    map = (XrtMapBacking *)owned; map_value(map, map->rows[0].key, true, 7);
    xrt_managed_drop(owned);
    CHECK(atomic_load(&strings.audit.finalized) == 1 && atomic_load(&maps.audit.finalized) == 1);
    CHECK(!atomic_load(&strings.audit.bytes) && !atomic_load(&maps.audit.bytes));
}
#include "backing_place_cases.h"
#include "backing_map_literal_cases.h"
#include "backing_array_local_cases.h"
#include "backing_array_region_cases.h"
#include "backing_array_inline_cases.h"
#include "backing_set_cases.h"
static void string_map_lifetimes(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    XrtManagedOps *ops = NULL;
    OK(xrt_map_string_value_ops(fixture.billing, &ops));
    CHECK(ops != fixture.types.map && xrt_map_string_ops_match(ops));
    CHECK(!xrt_map_string_ops_match(fixture.types.map));
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "key", 3, 0));
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, "key", 3, 0));
    OK(xrt_string_backing_create(fixture.frame, 2, &fixture.types, "old", 3, 0));
    static const char value[] = "x\0\xe4\xb8\x96";
    const size_t value_length = sizeof(value) - 1;
    OK(xrt_string_backing_create(fixture.frame, 3, &fixture.types, value, value_length, 0));
    OK(xrt_string_backing_create(fixture.frame, 4, &fixture.types, "", 0, 0));
    OK(xrt_string_backing_create(fixture.frame, 5, &fixture.types, "miss", 4, 0));
    XrtStringBacking *key = (XrtStringBacking *)borrow(&fixture, 0);
    XrtStringBacking *replacement_key = (XrtStringBacking *)borrow(&fixture, 1);
    XrtStringBacking *old = (XrtStringBacking *)borrow(&fixture, 2);
    XrtStringBacking *next = (XrtStringBacking *)borrow(&fixture, 3);
    XrtStringBacking *empty = (XrtStringBacking *)borrow(&fixture, 4);
    XrtStringBacking *miss = (XrtStringBacking *)borrow(&fixture, 5);
    XrtStringBacking *keys[] = {key, replacement_key, empty};
    XrtStringBacking *values[] = {old, next, empty};
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_map_string_create(fixture.frame, 6, ops, keys, values, 3) == XRT_MANAGED_OOM);
    same_bill(fixture.billing, baseline);
    CHECK(xr_signed_rc_read(&key->managed.references) == 1);
    CHECK(xr_signed_rc_read(&next->managed.references) == 1);
    CHECK(xr_signed_rc_read(&empty->managed.references) == 1);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    for (int32_t reserve = INT32_MAX; reserve >= INT32_MAX - 1; --reserve) {
        atomic_store(&next->managed.references, reserve);
        CHECK(xrt_map_string_create(fixture.frame, 6, ops, keys, values, 3) == XRT_MANAGED_REFCOUNT);
        same_bill(fixture.billing, baseline);
        CHECK(xr_signed_rc_read(&next->managed.references) == reserve);
        CHECK(xr_signed_rc_read(&key->managed.references) == 1);
        CHECK(xr_signed_rc_read(&old->managed.references) == 1);
    }
    atomic_store(&next->managed.references, 1);
    OK(xrt_map_string_create(fixture.frame, 6, ops, keys, values, 3));
    const XrtMapBacking *map = (XrtMapBacking *)borrow(&fixture, 6);
    CHECK(xrt_map_string_value_matches(&map->managed));
    CHECK(map->length == 2 && map->rows[0].key == key && map->rows[0].string_value == next);
    CHECK(map->rows[1].key == empty && map->rows[1].string_value == empty);
    CHECK(xr_signed_rc_read(&old->managed.references) == 1);
    CHECK(xr_signed_rc_read(&empty->managed.references) == 3);
    const XrtMapLiteralOrigin origins[] = {{0, 1, 5}, {1, 1, 8}, {0, 1, 15}};
    const XrtMapLiteral literal = {.items=origins, .count=3};
    XrtMapStringLiteralInput entries[] = {
        {.source.key=key, .value=old},
        {.source.spread=map},
        {.source.key=replacement_key, .value=next}
    };
    XrtMapLiteralDuplicate duplicate = {99, 99};
    XrtBillingStats before_literal = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_map_string_literal_create(fixture.frame, 9, ops, &literal, entries, &duplicate) == XRT_MANAGED_DUPLICATE_KEY);
    CHECK(duplicate.first == 0 && duplicate.second == 2);
    same_bill(fixture.billing, before_literal);
    const XrtMapLiteral spread = {.items=origins + 1, .count=2};
    entries[2].value = old;
    OK(xrt_map_string_literal_create(fixture.frame, 9, ops, &spread, entries + 1, &duplicate));
    const XrtMapBacking *spread_map = (XrtMapBacking *)borrow(&fixture, 9);
    CHECK(spread_map->length == 2 && spread_map->rows[0].key == key && spread_map->rows[0].string_value == old);
    CHECK(map->rows[0].string_value == next && spread_map->rows[1].string_value == empty);
    struct XrtMapLiteralInput cross_family[] = {{.source.spread=map}};
    const XrtMapLiteral one_spread = {.items=origins + 1, .count=1};
    BAD(xrt_map_backing_literal_create(fixture.frame, 10, &fixture.types, &one_spread, cross_family, &duplicate));
    OK(xrt_managed_slot_drop(fixture.frame, 9));
    same_bill(fixture.billing, before_literal);
    XrtStringBacking *old_keys[] = {key};
    int64_t old_values[] = {17};
    OK(xrt_map_backing_create(fixture.frame, 11, &fixture.types, old_keys, old_values, 1));
    const XrtMapBacking *scalar = (XrtMapBacking *)borrow(&fixture, 11);
    bool found = true;
    BAD(xrt_map_string_get(fixture.frame, 7, scalar, key, &found));
    CHECK(found);
    int64_t result = 83;
    BAD(xrt_map_backing_get(map, key, &result, &found));
    CHECK(found && result == 83);
    XrtMapUpdate update = {.operation=XRT_MAP_UPDATE_SET, .key=key, .value=63};
    bool applied = false, removed = true;
    BAD(xrt_map_backing_try_update((XrtMapBacking *)map, &update, &applied, &removed));
    CHECK(!applied && removed && map->rows[0].string_value == next);
    BAD(xrt_map_backing_update_create(fixture.frame, 10, &fixture.types, map, &update, &removed));
    BAD(xrt_map_backing_snapshot_held(fixture.frame, 10, &fixture.types, map, false));
    BAD(xrt_map_backing_display_held(fixture.frame, 10, &fixture.types, map));
    XrtBackingTypes wrong = fixture.types;
    wrong.map = ops;
    BAD(xrt_map_backing_create(fixture.frame, 10, &wrong, old_keys, old_values, 1));
    BAD(xrt_map_backing_literal_create(fixture.frame, 10, &wrong, &one_spread, cross_family, &duplicate));
    BAD(xrt_map_backing_update_create(fixture.frame, 10, &wrong, scalar, &update, &removed));
    OK(xrt_map_backing_get(scalar, key, &result, &found));
    CHECK(found && result == 17);
    OK(xrt_managed_slot_drop(fixture.frame, 11));
    OK(xrt_map_string_get(fixture.frame, 7, map, key, &found));
    CHECK(found && borrow(&fixture, 7) == &next->managed);
    OK(xrt_map_string_get(fixture.frame, 8, map, miss, &found));
    CHECK(!found);
    OK(xrt_map_string_get(fixture.frame, 8, map, empty, &found));
    CHECK(found && borrow(&fixture, 8) == &empty->managed);
    for (size_t i = 0; i < 7; ++i) OK(xrt_managed_slot_drop(fixture.frame, i));
    const XrtStringBacking *out = (XrtStringBacking *)borrow(&fixture, 7);
    CHECK(out->length == value_length && !memcmp(out->bytes, value, value_length));
    CHECK(xr_signed_rc_read(&out->managed.references) == 1);
    CHECK(((XrtStringBacking *)borrow(&fixture, 8))->length == 0);
    xrt_managed_ops_drop(ops);
    fixture_done(&fixture);
}
typedef struct StringMapWatch {
    Fixture *source, *destination;
    uintptr_t replaced_address;
    const XrtStringBacking *replacement;
    const XrtManagedHeader *prior;
    size_t allocations, retired_replaced;
} StringMapWatch;

static void string_map_observe(void *context, void *memory, bool allocating) {
    StringMapWatch *watch = context;
    XrtManagedHeader *current = borrow(watch->destination, 0);
    BAD(xrt_managed_root_drain(watch->destination->root));
    if (allocating) {
        CHECK(current == watch->prior && !watch->allocations);
        ++watch->allocations;
        OK(xrt_managed_root_drain(watch->source->root));
    } else if ((uintptr_t)memory == watch->replaced_address) {
        const XrtMapBacking *map = (const XrtMapBacking *)current;
        CHECK(map->length == 1);
        CHECK(map->rows[0].string_value == watch->replacement);
        ++watch->retired_replaced;
    }
}

static void string_map_callback_lifetimes(void) {
    Fixture source, destination;
    fixture_init(&source, SIZE_MAX);
    fixture_init(&destination, SIZE_MAX);
    XrtManagedOps *ops = NULL;
    OK(xrt_map_string_value_ops(destination.billing, &ops));
    OK(xrt_map_string_create(destination.frame, 0, ops, NULL, NULL, 0));
    XrtStringBacking *key = map_key(&source, 0, "key", 3);
    XrtStringBacking *old = map_key(&source, 1, "old", 3);
    XrtStringBacking *next = map_key(&source, 2, "new", 3);
    XrtStringBacking *keys[] = {key, key};
    XrtStringBacking *values[] = {old, next};
    StringMapWatch watch = {&source, &destination, (uintptr_t)old, next, borrow(&destination, 0), 0, 0};
    destination.audit.observe = source.audit.observe = string_map_observe;
    destination.audit.observer = source.audit.observer = &watch;
    OK(xrt_map_string_create(destination.frame, 0, ops, keys, values, 2));
    source.audit.observe = destination.audit.observe = NULL;
    CHECK(watch.allocations == 1 && watch.retired_replaced == 1);
    const XrtMapBacking *map = (const XrtMapBacking *)borrow(&destination, 0);
    bool present = false;
    OK(xrt_map_string_get(destination.frame, 1, map, key, &present));
    CHECK(present && borrow(&destination, 1) == &next->managed);
    xrt_managed_ops_drop(ops);
    fixture_retire(&source);
    CHECK(!atomic_load(&source.audit.finalized));
    fixture_done(&destination);
    CHECK(atomic_load(&source.audit.finalized) == 1);
}

int main(void) {
    string_map_lifetimes();
    string_map_callback_lifetimes();
    setup_failures();
    string_values();
    scalar_arrays();
    string_arrays();
    inline_array_rows();
    construction_failures();
    array_reserve_preserves_snapshots();
    pending_capacity();
    nonlocal_retirement();
    independent_lifetimes();
    scalar_updates();
    array_random_updates();
    local_i64_growth();
    array_local_updates();
    array_region_borrows();
    string_updates();
    sole_child_updates();
    repeat_and_get();
    array_displays();
    array_place_updates();
    array_place_guards();
    array_place_tokens();
    map_place_updates();
    map_local_edits();
    map_place_guards();
    map_construction_and_updates();
    map_cow_and_failures();
    map_snapshot_ownership();
    map_collisions_and_growth();
    map_retirement_and_lifetimes();
    map_literal_semantics();
    map_literal_rejections();
    map_literal_callback_lifetimes();
    map_literal_large_inputs();
    set_values_and_transactions();
    set_growth_and_lifetimes();
    puts("fixed String, Array, Map and Set backings: operations, rollback, order and retirement passed");
    return 0;
}
