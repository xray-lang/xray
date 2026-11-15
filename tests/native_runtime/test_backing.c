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
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
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
    for (size_t allowed = 0; allowed < 10; ++allowed) {
        XrtBackingTypes types = {0};
        xrt_billing_owner_fail_after(billing, allowed);
        CHECK(xrt_backing_types_init(billing, &types) == XRT_MANAGED_OOM);
        CHECK(!types.string);
        for (size_t i = 0; i < XRT_BACKING_ARRAY_COUNT; ++i) CHECK(!types.arrays[i]);
        xrt_backing_types_drop(&types);
        same_bill(billing, baseline);
    }
    xrt_billing_owner_fail_after(billing, 10);
    XrtBackingTypes types = {0};
    OK(xrt_backing_types_init(billing, &types));
    CHECK(xrt_billing_owner_stats(billing).blocks == baseline.blocks + 10);
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
    CHECK(after.bytes == before.bytes + 56 && after.blocks == before.blocks + 1);
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
    OK(xrt_managed_slot_prepare(fixture.frame, 0, fixture.types.string, 43, &pending));
    XrtStringBacking *manual = (XrtStringBacking *)pending;
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
    for (size_t i = 0; i < array->capacity; ++i) CHECK(tail(array, i) == (i < 3 ? &string->managed : NULL));
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
    for (size_t i = 0; i < empty->capacity; ++i) CHECK(!tail(empty, i));
    XrtStringBacking *invalid[] = {string, NULL};
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_array_backing_string_create(fixture.frame, 1, &fixture.types, invalid, 2, 4));
    CHECK(borrow(&fixture, 1) == &empty->managed);
    same_bill(fixture.billing, baseline);
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
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.types.arrays[XRT_BACKING_ARRAY_STRING], 32 + 5 * 8, &payload));
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    for (size_t i = 0; i < 5; ++i) CHECK(!tail(array, i));
    CHECK(xrt_managed_retain(string));
    memcpy(array->elements + 4 * sizeof(string), &string, sizeof(string));
    array->length = 0;
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
        OK(xrt_managed_slot_prepare(fixture->frame, 1, fixture->types.arrays[XRT_BACKING_ARRAY_STRING], 32 + 4 * 8, &payload));
        CHECK(xrt_managed_retain(string));
        XrtArrayBacking *array = (XrtArrayBacking *)payload;
        memcpy(array->elements + 3 * sizeof(string), &string, sizeof(string));
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
        CHECK(scalar_at(binding, kind, 0) == 17 && scalar_at(binding, kind, 1) == 23);
        edit.operation = XRT_ARRAY_UPDATE_PUSH; edit.item = scalar_item(kind, UINT64_MAX);
        OK(xrt_array_backing_update_create(fixture.frame, 0, &fixture.types, next, &edit));
        next = (XrtArrayBacking *)borrow(&fixture, 0);
        CHECK(next->length == 3 && next->capacity == 4);
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
    CHECK(!tail(next, 3) && !tail(array, 2) && !tail(array, 3));
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
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.types.string, 42, &pending));
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
        if (kind == XRT_BACKING_ARRAY_STRING)
            for (size_t i = 5; i < array->capacity; ++i) CHECK(!tail(array, i));
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
int main(void) {
    setup_failures();
    string_values();
    scalar_arrays();
    string_arrays();
    construction_failures();
    pending_capacity();
    nonlocal_retirement();
    independent_lifetimes();
    scalar_updates();
    string_updates();
    sole_child_updates();
    repeat_and_get();
    array_displays();
    puts("fixed String and four Array backings: construction, updates, reads, repeat, display and retirement passed");
    return 0;
}
