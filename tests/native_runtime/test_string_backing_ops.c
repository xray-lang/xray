/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_backing_ops.c - Typed text semantics and complete slot rollback
 *
 * KEY CONCEPT:
 *   Real allocators and owners expose aliases, partial split failures and exact
 *   refunds without a compiler value representation or guest ownership table.
 */
#include "runtime/native/xnative_string_backing_ops.h"
#include "runtime/native/xnative_array_backing.h"
#include "xray_native_values.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define STATUS(c, s) CHECK((c) == (s))

typedef struct Audit {
    _Atomic(size_t) leases, bytes, blocks, finalized, remaining, calls;
    XrtManagedRoot *probe_root;
    XrtManagedFrame *probe_frame;
    XrtManagedHeader *before_source, *before_result;
    size_t allocation_probes, retirement_probes, published_length;
    bool probe_allocate, probe_free;
    void (*observe)(void *, void *, bool);
    void *observation;
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
    size_t old = atomic_fetch_sub(&audit->leases, 1);
    CHECK(old);
    if (old == 1) {
        CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks));
        atomic_fetch_add(&audit->finalized, 1);
    }
}
static void provider_probe(Audit *audit, bool retiring) {
    if (!(retiring ? audit->probe_free : audit->probe_allocate)) return;
    STATUS(xrt_managed_root_drain(audit->probe_root), XRT_MANAGED_ARGUMENT);
    XrtManagedSlotLocation source, result, temporary;
    OK(xrt_managed_slot_location(audit->probe_frame, 0, &source));
    OK(xrt_managed_slot_location(audit->probe_frame, 1, &result));
    OK(xrt_managed_slot_location(audit->probe_frame, 2, &temporary));
    CHECK(*source.busy && *result.busy && *temporary.busy);
    if (retiring) {
        CHECK(!*source.owner && !*temporary.owner && *result.owner);
        XrtStringBacking *value = (XrtStringBacking *)*result.owner;
        CHECK(value->length == audit->published_length && !value->bytes[value->length]);
        ++audit->retirement_probes;
    } else {
        CHECK(*source.owner == audit->before_source && *result.owner == audit->before_result);
        ++audit->allocation_probes;
    }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = context;
    atomic_fetch_add(&audit->calls, 1);
    provider_probe(audit, false);
    if (audit->observe) audit->observe(audit->observation, NULL, false);
    size_t remaining = atomic_load(&audit->remaining);
    while (remaining != SIZE_MAX) {
        if (!remaining) return NULL;
        if (atomic_compare_exchange_weak(&audit->remaining, &remaining, remaining - 1)) break;
    }
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
    provider_probe(audit, true);
    if (audit->observe) audit->observe(audit->observation, memory, true);
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes);
    CHECK(atomic_fetch_sub(&audit->blocks, 1));
    xr_free(memory);
}
static void fixture_init(Fixture *fixture, size_t limit) {
    *fixture = (Fixture){0};
    atomic_init(&fixture->audit.leases, 1);
    atomic_init(&fixture->audit.remaining, SIZE_MAX);
    XrtManagedAllocator allocator = {&fixture->audit, provider_allocate, provider_free,
        {&fixture->audit, provider_retain, provider_drop}};
    OK(xrt_billing_owner_new(&allocator, limit, &fixture->billing));
    OK(xrt_backing_types_init(fixture->billing, &fixture->types));
    OK(xrt_managed_root_new(fixture->billing, &fixture->root));
    OK(xrt_managed_frame_enter(fixture->root, 24, &fixture->frame));
}
static void fixture_retire(Fixture *fixture) {
    OK(xrt_managed_root_destroy(&fixture->root));
    fixture->frame = NULL;
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
static XrtStringBacking *string(Fixture *fixture, size_t slot) {
    XrtStringBacking *value = (XrtStringBacking *)borrow(fixture, slot);
    CHECK(value);
    return value;
}
static XrtStringBacking *make(Fixture *fixture, size_t slot, const char *bytes) {
    OK(xrt_string_backing_create(fixture->frame, slot, &fixture->types, bytes, strlen(bytes), 0));
    return string(fixture, slot);
}
static void text_is(const XrtStringBacking *text, const char *bytes, size_t length, size_t runes) {
    CHECK(text && text->length == length && text->runes == runes && text->capacity > length);
    CHECK(!memcmp(text->bytes, bytes, length) && !text->bytes[length]);
}
static void slot_is(Fixture *fixture, size_t slot, const char *bytes) {
    text_is(string(fixture, slot), bytes, strlen(bytes), strlen(bytes));
}
static void same_bill(Fixture *fixture, XrtBillingStats baseline) {
    XrtBillingStats after = xrt_billing_owner_stats(fixture->billing);
    CHECK(after.bytes == baseline.bytes && after.blocks == baseline.blocks);
    CHECK(atomic_load(&fixture->audit.bytes) == after.bytes && atomic_load(&fixture->audit.blocks) == after.blocks);
}
static const XrtStringBacking *field(const XrtArrayBacking *array, size_t index) {
    const XrtStringBacking *value = NULL;
    OK(xrt_array_backing_string_at(array, index, &value));
    return value;
}
static void read_unicode(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    const char bytes[] = "A\xc3\xa9\xf0\x9f\x98\x80\0\xc3\xa9Z";
    OK(xrt_string_backing_create(f.frame, 0, &f.types, bytes, sizeof(bytes) - 1, 0));
    XrtStringBacking *text = string(&f, 0), *accent = make(&f, 1, "\xc3\xa9"), *empty = make(&f, 2, "");
    XrtStringBacking *suffix = make(&f, 3, "\xc3\xa9Z"), *start = make(&f, 4, "A");
    XrtStringBacking *long_text = make(&f, 5, "start:abcdefghij:end:abcdefghij");
    XrtStringBacking *long_pattern = make(&f, 6, "abcdefghij");
    const char binary_pattern[] = "\0\xc3\xa9";
    OK(xrt_string_backing_create(f.frame, 7, &f.types, binary_pattern, 3, 0));
    size_t calls = atomic_load(&f.audit.calls), length = SIZE_MAX, runes = SIZE_MAX;
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    OK(xrt_string_backing_counts(text, &length, &runes)); CHECK(length == 11 && runes == 6);
    int64_t found = 99;
    OK(xrt_string_backing_index_of(text, accent, 0, &found)); CHECK(found == 1);
    OK(xrt_string_backing_index_of(text, accent, 2, &found)); CHECK(found == 4);
    OK(xrt_string_backing_index_of(text, accent, 6, &found)); CHECK(found == -1);
    OK(xrt_string_backing_index_of(text, empty, 6, &found)); CHECK(found == 6);
    STATUS(xrt_string_backing_index_of(text, empty, -1, &found), XRT_MANAGED_BOUNDS); CHECK(found == 6);
    STATUS(xrt_string_backing_index_of(text, empty, 7, &found), XRT_MANAGED_BOUNDS); CHECK(found == 6);
    OK(xrt_string_backing_last_index_of(text, accent, &found)); CHECK(found == 4);
    OK(xrt_string_backing_last_index_of(text, empty, &found)); CHECK(found == 6);
    OK(xrt_string_backing_last_index_of(text, long_pattern, &found)); CHECK(found == -1);
    OK(xrt_string_backing_index_of(text, string(&f, 7), 0, &found)); CHECK(found == 3);
    OK(xrt_string_backing_index_of(long_text, long_pattern, 0, &found)); CHECK(found == 6);
    OK(xrt_string_backing_last_index_of(long_text, long_pattern, &found)); CHECK(found == 21);
    bool yes = false;
    OK(xrt_string_backing_test(text, accent, XRT_STRING_CONTAINS, &yes)); CHECK(yes);
    OK(xrt_string_backing_test(text, start, XRT_STRING_STARTS_WITH, &yes)); CHECK(yes);
    OK(xrt_string_backing_test(text, suffix, XRT_STRING_ENDS_WITH, &yes)); CHECK(yes);
    OK(xrt_string_backing_test(text, accent, XRT_STRING_ENDS_WITH, &yes)); CHECK(!yes);
    OK(xrt_string_backing_test(empty, empty, XRT_STRING_CONTAINS, &yes)); CHECK(yes);
    STATUS(xrt_string_backing_test(text, accent, (XrtStringTest)99, &yes), XRT_MANAGED_ARGUMENT); CHECK(yes);
    int order = 99;
    OK(xrt_string_backing_compare(text, text, &order)); CHECK(!order);
    OK(xrt_string_backing_compare(start, text, &order)); CHECK(order == -1);
    OK(xrt_string_backing_compare(accent, start, &order)); CHECK(order == 1);
    uint32_t expected[] = {'A', 0xe9, 0x1f600, 0, 0xe9, 'Z'}, rune = 99;
    size_t cursor = 0;
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        OK(xrt_string_backing_rune_at(text, (int64_t)i, &rune)); CHECK(rune == expected[i]);
        OK(xrt_string_backing_rune_next(text, &cursor, &rune, &yes)); CHECK(yes && rune == expected[i]);
    }
    OK(xrt_string_backing_rune_next(text, &cursor, &rune, &yes)); CHECK(!yes && rune == 'Z' && cursor == 11);
    STATUS(xrt_string_backing_rune_at(text, 6, &rune), XRT_MANAGED_BOUNDS); CHECK(rune == 'Z');
    STATUS(xrt_string_backing_rune_at(text, -1, &rune), XRT_MANAGED_BOUNDS);
    cursor = 2; yes = true;
    STATUS(xrt_string_backing_rune_next(text, &cursor, &rune, &yes), XRT_MANAGED_BOUNDS);
    CHECK(cursor == 2 && rune == 'Z' && yes);
    cursor = 12;
    STATUS(xrt_string_backing_rune_next(text, &cursor, &rune, &yes), XRT_MANAGED_BOUNDS);
    CHECK(cursor == 12);
    CHECK(atomic_load(&f.audit.calls) == calls);
    same_bill(&f, baseline);
    fixture_done(&f);
    puts("String fixed-layout reads, Unicode search and borrowed runes passed");
}
static void derived_alias_chain(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *text = make(&f, 0, "\xce\xb1\xce\xb2\xce\xb1"), *pattern = make(&f, 1, "\xce\xb1");
    XrtStringBacking *replacement = make(&f, 2, "xy"), *empty = make(&f, 3, "");
    OK(xrt_string_backing_slice_create(f.frame, 4, &f.types, text, 1, 3));
    text_is(string(&f, 4), "\xce\xb2\xce\xb1", 4, 2);
    OK(xrt_string_backing_slice_create(f.frame, 4, &f.types, string(&f, 4), 0, 1));
    text_is(string(&f, 4), "\xce\xb2", 2, 1);
    OK(xrt_string_backing_slice_create(f.frame, 5, &f.types, text, 3, 3)); slot_is(&f, 5, "");
    XrtManagedHeader *old = borrow(&f, 4);
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_slice_create(f.frame, 4, &f.types, text, -1, 2), XRT_MANAGED_BOUNDS);
    STATUS(xrt_string_backing_slice_create(f.frame, 4, &f.types, text, 2, 1), XRT_MANAGED_BOUNDS);
    STATUS(xrt_string_backing_slice_create(f.frame, 4, &f.types, text, 0, 4), XRT_MANAGED_BOUNDS);
    CHECK(borrow(&f, 4) == old); same_bill(&f, baseline);
    OK(xrt_string_backing_repeat_create(f.frame, 0, &f.types, text, 3));
    text = string(&f, 0); text_is(text, "\xce\xb1\xce\xb2\xce\xb1\xce\xb1\xce\xb2\xce\xb1\xce\xb1\xce\xb2\xce\xb1", 18, 9);
    OK(xrt_string_backing_replace_create(f.frame, 0, &f.types, text, (XrtStringReplacement){pattern, replacement, true}));
    text = string(&f, 0); text_is(text, "xy\xce\xb2xyxy\xce\xb2xyxy\xce\xb2xy", 18, 15);
    OK(xrt_string_backing_replace_create(f.frame, 6, &f.types, text, (XrtStringReplacement){empty, pattern, true}));
    CHECK(string(&f, 6) == text && xr_signed_rc_read(&text->managed.references) == 2);
    OK(xrt_string_backing_copy_into(f.frame, 0, text));
    CHECK(string(&f, 0) == text && xr_signed_rc_read(&text->managed.references) == 2);
    OK(xrt_string_backing_repeat_create(f.frame, 7, &f.types, text, 1)); CHECK(string(&f, 7) == text);
    OK(xrt_string_backing_repeat_create(f.frame, 8, &f.types, text, 0)); slot_is(&f, 8, "");
    OK(xrt_string_backing_repeat_create(f.frame, 8, &f.types, text, INT64_MIN)); slot_is(&f, 8, "");
    OK(xrt_string_backing_repeat_create(f.frame, 8, &f.types, empty, INT64_MAX)); slot_is(&f, 8, "");
    OK(xrt_string_backing_concat_create(f.frame, 1, &f.types, pattern, pattern, 0));
    text_is(string(&f, 1), "\xce\xb1\xce\xb1", 4, 2);
    XrtStringBacking *overlap = make(&f, 9, "aaaaa"), *pair = make(&f, 10, "aa"), *one = make(&f, 11, "b");
    OK(xrt_string_backing_replace_create(f.frame, 12, &f.types, overlap, (XrtStringReplacement){pair, one, true})); slot_is(&f, 12, "bba");
    OK(xrt_string_backing_replace_create(f.frame, 12, &f.types, overlap, (XrtStringReplacement){pair, empty, false})); slot_is(&f, 12, "aaa");
    OK(xrt_string_backing_replace_create(f.frame, 12, &f.types, overlap, (XrtStringReplacement){pair, pair, true})); CHECK(string(&f, 12) == overlap);
    OK(xrt_string_backing_replace_create(f.frame, 12, &f.types, overlap, (XrtStringReplacement){one, empty, true})); CHECK(string(&f, 12) == overlap);
    OK(xrt_string_backing_copy_bytes_create(f.frame, 0, &f.types, text));
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&f, 0);
    CHECK(array->length == 18 && array->capacity == 18 && !memcmp(array->elements, text->bytes, 18));
    CHECK(xr_signed_rc_read(&text->managed.references) == 2);
    fixture_done(&f);
    puts("String slice, repeat, replacement and aliased publication passed");
}
static void exclusive_append(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    OK(xrt_string_backing_create(f.frame, 0, &f.types, "\xc3\xa9", 2, 17));
    XrtStringBacking *text = string(&f, 0), *suffix = make(&f, 1, "!");
    size_t capacity = text->capacity;
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    size_t calls = atomic_load(&f.audit.calls);
    bool appended = false;
    OK(xrt_string_backing_try_append_exclusive(text, text, &appended)); CHECK(appended);
    text_is(text, "\xc3\xa9\xc3\xa9", 4, 2);
    atomic_store(&text->managed.references, -1);
    OK(xrt_string_backing_try_append_exclusive(text, suffix, &appended)); CHECK(appended);
    CHECK(xr_signed_rc_read(&text->managed.references) == 1);
    text_is(text, "\xc3\xa9\xc3\xa9!", 5, 3);
    OK(xrt_string_backing_copy_into(f.frame, 2, text));
    OK(xrt_string_backing_try_append_exclusive(text, suffix, &appended)); CHECK(!appended);
    text_is(text, "\xc3\xa9\xc3\xa9!", 5, 3);
    OK(xrt_managed_slot_drop(f.frame, 2));
    atomic_store(&text->managed.references, -1);
    OK(xrt_string_backing_copy_into(f.frame, 2, text));
    OK(xrt_string_backing_try_append_exclusive(text, suffix, &appended)); CHECK(!appended);
    CHECK(xr_signed_rc_read(&text->managed.references) == -2);
    OK(xrt_managed_slot_drop(f.frame, 2));
    CHECK(atomic_load(&f.audit.calls) == calls && text->capacity == capacity);
    same_bill(&f, baseline);
    XrtStringBacking *tight = make(&f, 3, "q");
    atomic_store(&tight->managed.references, -1);
    OK(xrt_string_backing_try_append_exclusive(tight, suffix, &appended)); CHECK(!appended);
    CHECK(xr_signed_rc_read(&tight->managed.references) == -1);
    OK(xrt_string_backing_concat_create(f.frame, 3, &f.types, tight, suffix, 16)); slot_is(&f, 3, "q!");
    fixture_done(&f);
    puts("String exclusive append, shared misses and COW fallback passed");
}
static void formatted_values(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    OK(xrt_string_backing_i64_create(f.frame, 0, &f.types, INT64_MIN)); slot_is(&f, 0, "-9223372036854775808");
    OK(xrt_string_backing_i64_create(f.frame, 0, &f.types, INT64_MAX)); slot_is(&f, 0, "9223372036854775807");
    OK(xrt_string_backing_u64_create(f.frame, 0, &f.types, UINT64_MAX)); slot_is(&f, 0, "18446744073709551615");
    OK(xrt_string_backing_u64_create(f.frame, 0, &f.types, 0)); slot_is(&f, 0, "0");
    OK(xrt_string_backing_bool_create(f.frame, 0, &f.types, true)); slot_is(&f, 0, "true");
    OK(xrt_string_backing_bool_create(f.frame, 0, &f.types, false)); slot_is(&f, 0, "false");
    OK(xrt_string_backing_rune_create(f.frame, 0, &f.types, 0x1f600)); text_is(string(&f, 0), "\xf0\x9f\x98\x80", 4, 1);
    OK(xrt_string_backing_rune_create(f.frame, 0, &f.types, 0)); text_is(string(&f, 0), "\0", 1, 1);
    XrtManagedHeader *old = borrow(&f, 0);
    STATUS(xrt_string_backing_rune_create(f.frame, 0, &f.types, 0xd800), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_string_backing_rune_create(f.frame, 0, &f.types, 0x110000), XRT_MANAGED_ARGUMENT); CHECK(borrow(&f, 0) == old);
    static const struct { uint64_t bits; const char *text; } floats[] = {
        {0, "0.0"}, {UINT64_C(0x8000000000000000), "-0.0"},
        {UINT64_C(0x3fb999999999999a), "0.1"}, {UINT64_C(0x3ff0000000000000), "1.0"},
        {1, "5e-324"}, {UINT64_C(0x0010000000000000), "2.2250738585072014e-308"},
        {UINT64_C(0x7fefffffffffffff), "1.7976931348623157e+308"},
        {UINT64_C(0x7ff0000000000000), "inf"}, {UINT64_C(0xfff0000000000000), "-inf"},
        {UINT64_C(0xfff8000000001234), "nan"}, {UINT64_C(0x7ff0000000000001), "nan"}
    };
    for (size_t i = 0; i < sizeof(floats) / sizeof(*floats); ++i) {
        OK(xrt_string_backing_f64_create(f.frame, 0, &f.types, floats[i].bits)); slot_is(&f, 0, floats[i].text);
    }
    fixture_done(&f);
    puts("String canonical integer, rune and shortest binary64 formatting passed");
}
static void pod_display_case(Fixture *fixture, XrPodScalar scalar, const void *value, const char *expected) {
    XrPodLayout primitive = {0}; CHECK(xr_pod_scalar_layout(scalar, &primitive));
    for (uint32_t depth = 0; depth < 4; ++depth) {
        XrtPodDisplay description = {.scalar=scalar, .nullable_depth=depth, .physical=primitive};
        for (uint32_t n = 0; n < depth; ++n) CHECK(xr_pod_nullable_layout(description.physical, &description.physical));
        unsigned char bytes[32] = {0};
        for (uint32_t n = 0; n < depth; ++n) bytes[n * primitive.alignment] = 1;
        if (primitive.size) memcpy(bytes + depth * primitive.alignment, value, primitive.size);
        OK(xrt_string_backing_pod_display_create(fixture->frame, 0, &fixture->types, &description,
            description.physical.size ? bytes : NULL));
        slot_is(fixture, 0, expected);
        for (uint32_t n = 0; n < depth; ++n) {
            bytes[n * primitive.alignment] = 0;
            OK(xrt_string_backing_pod_display_create(fixture->frame, 0, &fixture->types, &description, bytes));
            slot_is(fixture, 0, "null"); bytes[n * primitive.alignment] = 1;
        }
        XrtManagedHeader *old = borrow(fixture, 0);
        XrtBillingStats baseline = xrt_billing_owner_stats(fixture->billing);
        ++description.physical.size;
        STATUS(xrt_string_backing_pod_display_create(fixture->frame, 0, &fixture->types, &description, bytes), XRT_MANAGED_ARGUMENT);
        --description.physical.size;
        if (depth) {
            bytes[0] = 2;
            STATUS(xrt_string_backing_pod_display_create(fixture->frame, 0, &fixture->types, &description, bytes), XRT_MANAGED_ARGUMENT);
        }
        CHECK(borrow(fixture, 0) == old); same_bill(fixture, baseline);
    }
}
static void pod_display_values(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    pod_display_case(&f, XR_POD_UNIT, NULL, "()");
#define POD_CASE(kind, type, bits, text) do { type value = bits; pod_display_case(&f, kind, &value, text); } while (0)
    POD_CASE(XR_POD_BOOL, uint8_t, 0, "false"); POD_CASE(XR_POD_BOOL, uint8_t, 1, "true");
    POD_CASE(XR_POD_I8, int8_t, INT8_MIN, "-128"); POD_CASE(XR_POD_I16, int16_t, INT16_MIN, "-32768");
    POD_CASE(XR_POD_I32, int32_t, INT32_MIN, "-2147483648");
    POD_CASE(XR_POD_I64, int64_t, INT64_MIN, "-9223372036854775808");
    POD_CASE(XR_POD_U8, uint8_t, UINT8_MAX, "255"); POD_CASE(XR_POD_U16, uint16_t, UINT16_MAX, "65535");
    POD_CASE(XR_POD_U32, uint32_t, UINT32_MAX, "4294967295");
    POD_CASE(XR_POD_U64, uint64_t, UINT64_MAX, "18446744073709551615");
    POD_CASE(XR_POD_F32, uint32_t, UINT32_C(0x3fa00000), "1.25");
    POD_CASE(XR_POD_F32, uint32_t, UINT32_C(0x7f800001), "nan");
    POD_CASE(XR_POD_F64, uint64_t, UINT64_C(0x8000000000000000), "-0.0");
    POD_CASE(XR_POD_F64, uint64_t, UINT64_C(0x7ff0000000000001), "nan");
#undef POD_CASE
    XrtPodDisplay description = {.scalar=XR_POD_BOOL, .physical={1, 1, 1}};
    uint8_t value = 2;
    XrtManagedHeader *old = borrow(&f, 0); XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_pod_display_create(f.frame, 0, &f.types, &description, &value), XRT_MANAGED_ARGUMENT);
    value = 1; atomic_store(&f.audit.remaining, 0);
    STATUS(xrt_string_backing_pod_display_create(f.frame, 0, &f.types, &description, &value), XRT_MANAGED_OOM);
    CHECK(borrow(&f, 0) == old); same_bill(&f, baseline); atomic_store(&f.audit.remaining, SIZE_MAX);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 0, f.types.string, 42, &pending));
    XrtManagedSlotLocation location; OK(xrt_managed_slot_location(f.frame, 0, &location));
    baseline = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_pod_display_create(f.frame, 0, &f.types, &description, &value), XRT_MANAGED_ARGUMENT);
    CHECK(location.pending->value == pending && borrow(&f, 0) == old); same_bill(&f, baseline);
    OK(xrt_managed_slot_abort(f.frame, 0)); fixture_done(&f);
    puts("POD primitive and nested Nullable display preserves exact bytes and owner rollback");
}
static void split_values(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *text = make(&f, 0, ":\xc3\xa9::\xf0\x9f\x98\x80:"), *separator = make(&f, 1, ":");
    XrtStringBacking *empty = make(&f, 2, ""), *missing = make(&f, 3, "x"), *unicode = make(&f, 4, "\xc3\xa9");
    XrtStringSplitResult result = xrt_string_backing_split_create(f.frame, 0, &f.types, text, separator, 23);
    OK(result.status); CHECK(!result.empty_pattern && !borrow(&f, 23));
    XrtArrayBacking *array = (XrtArrayBacking *)borrow(&f, 0);
    CHECK(array->length == 5 && array->capacity == 5);
    text_is(field(array, 0), "", 0, 0); text_is(field(array, 1), "\xc3\xa9", 2, 1);
    text_is(field(array, 2), "", 0, 0); text_is(field(array, 3), "\xf0\x9f\x98\x80", 4, 1); text_is(field(array, 4), "", 0, 0);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, empty, separator, 23); OK(result.status);
    array = (XrtArrayBacking *)borrow(&f, 5); CHECK(array->length == 1); text_is(field(array, 0), "", 0, 0);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, unicode, missing, 23); OK(result.status);
    array = (XrtArrayBacking *)borrow(&f, 5); CHECK(array->length == 1); text_is(field(array, 0), "\xc3\xa9", 2, 1);
    XrtManagedHeader *old = borrow(&f, 5);
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, unicode, empty, 23);
    OK(result.status); CHECK(result.empty_pattern && borrow(&f, 5) == old && !borrow(&f, 23)); same_bill(&f, baseline);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, empty, empty, 23);
    OK(result.status); CHECK(result.empty_pattern); same_bill(&f, baseline);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, unicode, separator, 5); STATUS(result.status, XRT_MANAGED_ARGUMENT);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, unicode, separator, 2); STATUS(result.status, XRT_MANAGED_ARGUMENT);
    CHECK(borrow(&f, 5) == old && borrow(&f, 2) == &empty->managed); same_bill(&f, baseline);
    XrtManagedHeader *payload = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 23, f.types.string, 44, &payload));
    XrtStringBacking *pending = (XrtStringBacking *)payload; pending->length = 3; pending->runes = 3; memcpy(pending->bytes, "own", 4);
    XrtBillingStats with_pending = xrt_billing_owner_stats(f.billing);
    result = xrt_string_backing_split_create(f.frame, 5, &f.types, unicode, separator, 23); STATUS(result.status, XRT_MANAGED_ARGUMENT);
    CHECK(borrow(&f, 5) == old && !borrow(&f, 23)); same_bill(&f, with_pending);
    OK(xrt_managed_slot_commit(f.frame, 23)); CHECK(borrow(&f, 23) == payload); slot_is(&f, 23, "own");
    OK(xrt_managed_slot_drop(f.frame, 23)); same_bill(&f, baseline);
    text = make(&f, 6, "\xc3\xa9\xc3\xa9x\xc3\xa9");
    result = xrt_string_backing_split_create(f.frame, 7, &f.types, text, unicode, 23); OK(result.status);
    array = (XrtArrayBacking *)borrow(&f, 7); CHECK(array->length == 4);
    text_is(field(array, 0), "", 0, 0); text_is(field(array, 1), "", 0, 0);
    text_is(field(array, 2), "x", 1, 1); text_is(field(array, 3), "", 0, 0);
    fixture_done(&f);
    puts("String split empty patterns, Unicode fields and foreign pending rejection passed");
}
static void failures_and_refunds(void) {
    Fixture f; fixture_init(&f, 32768);
    XrtStringBacking *text = make(&f, 0, "a:b:c:d"), *separator = make(&f, 1, ":"), *replacement = make(&f, 2, "XYZ");
    XrtManagedHeader *old = &make(&f, 3, "old")->managed;
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    size_t calls = atomic_load(&f.audit.calls);
    STATUS(xrt_string_backing_repeat_create(f.frame, 3, &f.types, text, INT64_MAX), XRT_MANAGED_OVERFLOW);
    STATUS(xrt_string_backing_create(f.frame, 3, &f.types, "\xc0\x80", 2, 0), XRT_MANAGED_BAD_UTF8);
    CHECK(borrow(&f, 3) == old && atomic_load(&f.audit.calls) == calls); same_bill(&f, baseline);
    STATUS(xrt_string_backing_repeat_create(f.frame, 3, &f.types, text, 10000), XRT_MANAGED_LIMIT);
    CHECK(atomic_load(&f.audit.calls) == calls && borrow(&f, 3) == old); same_bill(&f, baseline);
    atomic_store(&f.audit.remaining, 0);
    STATUS(xrt_string_backing_slice_create(f.frame, 3, &f.types, text, 0, 1), XRT_MANAGED_OOM);
    STATUS(xrt_string_backing_repeat_create(f.frame, 3, &f.types, text, 2), XRT_MANAGED_OOM);
    STATUS(xrt_string_backing_replace_create(f.frame, 3, &f.types, text, (XrtStringReplacement){separator, replacement, true}), XRT_MANAGED_OOM);
    STATUS(xrt_string_backing_copy_bytes_create(f.frame, 3, &f.types, text), XRT_MANAGED_OOM);
    STATUS(xrt_string_backing_i64_create(f.frame, 3, &f.types, -1), XRT_MANAGED_OOM);
    STATUS(xrt_string_backing_f64_create(f.frame, 3, &f.types, 1), XRT_MANAGED_OOM);
    CHECK(borrow(&f, 3) == old); same_bill(&f, baseline);
    for (size_t allowed = 0; allowed < 5; ++allowed) {
        atomic_store(&f.audit.remaining, allowed);
        XrtStringSplitResult result = xrt_string_backing_split_create(f.frame, 3, &f.types, text, separator, 23);
        STATUS(result.status, XRT_MANAGED_OOM); CHECK(!result.empty_pattern && borrow(&f, 3) == old && !borrow(&f, 23));
        same_bill(&f, baseline);
    }
    atomic_store(&f.audit.remaining, 5);
    XrtStringSplitResult result = xrt_string_backing_split_create(f.frame, 3, &f.types, text, separator, 23);
    OK(result.status); CHECK(!borrow(&f, 23) && ((XrtArrayBacking *)borrow(&f, 3))->length == 4);
    atomic_store(&f.audit.remaining, SIZE_MAX);
    OK(xrt_managed_slot_drop(f.frame, 3)); old = &make(&f, 3, "old")->managed;
    baseline = xrt_billing_owner_stats(f.billing);
    atomic_store(&text->managed.references, INT32_MAX);
    STATUS(xrt_string_backing_copy_into(f.frame, 3, text), XRT_MANAGED_REFCOUNT);
    STATUS(xrt_string_backing_repeat_create(f.frame, 3, &f.types, text, 1), XRT_MANAGED_REFCOUNT);
    CHECK(borrow(&f, 3) == old); atomic_store(&text->managed.references, 1); same_bill(&f, baseline);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 3, f.types.string, 42, &pending));
    XrtStringBacking *partial = (XrtStringBacking *)pending;
    partial->length = partial->runes = 1; memcpy(partial->bytes, "p", 2);
    XrtBillingStats with_pending = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_copy_into(f.frame, 3, text), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_string_backing_slice_create(f.frame, 3, &f.types, text, 0, 1), XRT_MANAGED_ARGUMENT);
    result = xrt_string_backing_split_create(f.frame, 3, &f.types, text, separator, 23); STATUS(result.status, XRT_MANAGED_ARGUMENT);
    CHECK(borrow(&f, 3) == old && xr_signed_rc_read(&text->managed.references) == 1); same_bill(&f, with_pending);
    OK(xrt_managed_slot_commit(f.frame, 3)); CHECK(borrow(&f, 3) == pending); slot_is(&f, 3, "p");
    xrt_billing_owner_close(f.billing);
    STATUS(xrt_string_backing_slice_create(f.frame, 3, &f.types, text, 0, 1), XRT_MANAGED_CLOSED);
    OK(xrt_string_backing_copy_into(f.frame, 3, text)); CHECK(borrow(&f, 3) == &text->managed);
    fixture_done(&f);
    puts("String overflow, UTF8, quota, provider OOM and partial split refunds passed");
}
static void independent_owners(void) {
    Fixture source, destination; fixture_init(&source, SIZE_MAX); fixture_init(&destination, SIZE_MAX);
    XrtStringBacking *text = make(&source, 0, "\xc3\xa9:value"), *separator = make(&source, 1, ":");
    OK(xrt_string_backing_copy_into(destination.frame, 0, text));
    XrtStringSplitResult result = xrt_string_backing_split_create(destination.frame, 1, &destination.types, text, separator, 23);
    OK(result.status);
    fixture_retire(&source);
    CHECK(!atomic_load(&source.audit.finalized));
    text_is(string(&destination, 0), "\xc3\xa9:value", 8, 7);
    XrtManagedHeader *escaped = NULL;
    OK(xrt_managed_slot_take(destination.frame, 1, &escaped));
    OK(xrt_managed_slot_drop(destination.frame, 0)); CHECK(atomic_load(&source.audit.finalized) == 1);
    fixture_retire(&destination); CHECK(!atomic_load(&destination.audit.finalized));
    XrtArrayBacking *array = (XrtArrayBacking *)escaped;
    CHECK(array->length == 2); text_is(field(array, 0), "\xc3\xa9", 2, 1); text_is(field(array, 1), "value", 5, 5);
    xrt_managed_drop(escaped);
    CHECK(atomic_load(&destination.audit.finalized) == 1 && !atomic_load(&destination.audit.bytes));
    puts("String cross-owner copy and escaped split metadata lifetimes passed");
}
static void owned_append_aliases(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    OK(xrt_string_backing_create(f.frame, 0, &f.types, "ab", 2, 64));
    XrtStringBacking *left = string(&f, 0), *suffix = make(&f, 3, "c");
    make(&f, 1, "old");
    size_t calls = atomic_load(&f.audit.calls);
    XrtStringAppendOwned input = {0,1,2,suffix};
    OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
    CHECK(!borrow(&f, 0) && !borrow(&f, 2) && string(&f, 1) == left);
    CHECK(atomic_load(&f.audit.calls) == calls && xr_signed_rc_read(&left->managed.references) == 1);
    slot_is(&f, 1, "abc");
    input = (XrtStringAppendOwned){1,0,2,left};
    OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
    CHECK(!borrow(&f, 1) && string(&f, 0) == left && atomic_load(&f.audit.calls) == calls);
    slot_is(&f, 0, "abcabc");
    OK(xrt_string_backing_copy_into(f.frame, 1, left));
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    atomic_store(&f.audit.remaining, 0);
    input = (XrtStringAppendOwned){0,1,2,left};
    STATUS(xrt_string_backing_append_owned(f.frame, &f.types, &input), XRT_MANAGED_OOM);
    CHECK(borrow(&f, 0) == &left->managed && borrow(&f, 1) == &left->managed && !borrow(&f, 2));
    slot_is(&f, 0, "abcabc"); same_bill(&f, before);
    atomic_store(&f.audit.remaining, SIZE_MAX);
    OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
    CHECK(!borrow(&f, 0) && !borrow(&f, 2) && string(&f, 1) != left);
    slot_is(&f, 1, "abcabcabcabc");
    fixture_done(&f);
}
static void owned_append_preflight(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    OK(xrt_string_backing_create(f.frame, 0, &f.types, "p", 1, 64));
    XrtStringBacking *source = string(&f, 0), *old = make(&f, 1, "old"), *suffix = make(&f, 3, "z");
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 1, f.types.string, sizeof(XrtStringBacking) + 2, &pending));
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    XrtStringAppendOwned input = {0,1,2,suffix};
    STATUS(xrt_string_backing_append_owned(f.frame, &f.types, &input), XRT_MANAGED_ARGUMENT);
    CHECK(borrow(&f, 0) == &source->managed && borrow(&f, 1) == &old->managed && !borrow(&f, 2));
    slot_is(&f, 0, "p"); same_bill(&f, before);
    OK(xrt_managed_slot_abort(f.frame, 1));
    input.result_slot = 0;
    STATUS(xrt_string_backing_append_owned(f.frame, &f.types, &input), XRT_MANAGED_ARGUMENT);
    input.result_slot = 1; make(&f, 2, "occupied");
    STATUS(xrt_string_backing_append_owned(f.frame, &f.types, &input), XRT_MANAGED_ARGUMENT);
    slot_is(&f, 0, "p"); slot_is(&f, 1, "old"); slot_is(&f, 2, "occupied");
    OK(xrt_managed_slot_drop(f.frame, 1));
    OK(xrt_managed_slot_drop(f.frame, 2));
    pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 2, f.types.string, sizeof(XrtStringBacking) + 2, &pending));
    before = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_append_owned(f.frame, &f.types, &input), XRT_MANAGED_ARGUMENT);
    CHECK(!borrow(&f, 1) && !borrow(&f, 2)); slot_is(&f, 0, "p"); same_bill(&f, before);
    OK(xrt_managed_slot_abort(f.frame, 2));
    size_t calls = atomic_load(&f.audit.calls);
    OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
    CHECK(!borrow(&f, 0) && !borrow(&f, 2) && string(&f, 1) == source);
    CHECK(atomic_load(&f.audit.calls) == calls);
    slot_is(&f, 1, "pz");
    fixture_done(&f);
}
static void append_capacity_window(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    size_t empty[] = {1, 2};
    OK(xrt_string_backing_create(f.frame, 0, &f.types, "p", 1, 11));
    OK(xrt_string_backing_create(f.frame, 3, &f.types, "\xc3\xa9\0", 3, 0));
    XrtStringBacking *text = string(&f, 0);
    OK(xrt_string_backing_copy_into(f.frame, 1, text));
    CHECK(!xrt_native_string_append_window(f.frame, 0, 3, 8, NULL, 0));
    CHECK(!xrt_native_string_append_window(f.frame, 0, 3, 8, empty, 2));
    OK(xrt_managed_slot_drop(f.frame, 1));
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 2, f.types.string, sizeof(XrtStringBacking) + 2, &pending));
    CHECK(!xrt_native_string_append_window(f.frame, 0, 3, 8, empty, 2));
    CHECK(text->length == 1 && text->runes == 1);
    OK(xrt_managed_slot_abort(f.frame, 2));
    XrtManagedSlotLocation location;
    OK(xrt_managed_slot_location(f.frame, 0, &location));
    OK(xrt_managed_domain_enter(location));
    CHECK(!xrt_native_string_append_window(f.frame, 0, 3, 8, empty, 2));
    xrt_managed_domain_leave(location);
    size_t calls = atomic_load(&f.audit.calls);
    CHECK(xrt_native_string_append_window(f.frame, 0, 3, 8, empty, 2) == 3);
    CHECK(string(&f, 0) == text && atomic_load(&f.audit.calls) == calls);
    text_is(text, "p\xc3\xa9\0\xc3\xa9\0\xc3\xa9\0", 10, 7);
    CHECK(!xrt_native_string_append_window(f.frame, 0, 3, 8, empty, 2));
    fixture_done(&f);
}

static void owned_append_growth_and_callbacks(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    make(&f, 0, "a"); make(&f, 1, "old"); XrtStringBacking *suffix = make(&f, 3, "z");
    f.audit.probe_root = f.root; f.audit.probe_frame = f.frame;
    f.audit.before_source = borrow(&f, 0); f.audit.before_result = borrow(&f, 1);
    f.audit.published_length = 2; f.audit.probe_allocate = f.audit.probe_free = true;
    XrtStringAppendOwned input = {0,1,2,suffix};
    OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
    f.audit.probe_allocate = f.audit.probe_free = false;
    CHECK(f.audit.allocation_probes && f.audit.retirement_probes >= 2);
    slot_is(&f, 1, "az");
    size_t calls = atomic_load(&f.audit.calls), source = 1;
    for (size_t i = 0; i < 4096; ++i) {
        input = (XrtStringAppendOwned){source,1-source,2,suffix};
        OK(xrt_string_backing_append_owned(f.frame, &f.types, &input));
        source = 1-source;
        CHECK(!borrow(&f, 1-source) && !borrow(&f, 2));
    }
    XrtStringBacking *value = string(&f, source);
    CHECK(value->length == 4098 && value->runes == 4098 && atomic_load(&f.audit.calls) - calls < 16);
    CHECK(value->bytes[0] == 'a' && !value->bytes[4098]);
    for (size_t i = 1; i < value->length; ++i) CHECK(value->bytes[i] == 'z');
    fixture_done(&f);
}
static XrtPlace binding(Fixture *fixture, size_t slot) {
    return (XrtPlace){.frame=fixture->frame,.slot=slot,.nulls=XRT_PLACE_NONNULL};
}
static void binding_append_aliases(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    OK(xrt_string_backing_create(f.frame, 0, &f.types, "a\0\xc3\xa9", 4, 64));
    XrtStringBacking *left = string(&f, 0), *suffix = make(&f, 3, "z");
    size_t calls = atomic_load(&f.audit.calls);
    OK(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix));
    CHECK(string(&f, 0) == left && !borrow(&f, 2) && atomic_load(&f.audit.calls) == calls);
    text_is(left, "a\0\xc3\xa9z", 5, 4);
    OK(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, left));
    CHECK(string(&f, 0) == left && atomic_load(&f.audit.calls) == calls &&
        xr_signed_rc_read(&left->managed.references) == 1);
    text_is(left, "a\0\xc3\xa9z" "a\0\xc3\xa9z", 10, 8);
    OK(xrt_string_backing_copy_into(f.frame, 1, left));
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    int32_t references = xr_signed_rc_read(&left->managed.references);
    atomic_store(&f.audit.remaining, 0);
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, left), XRT_MANAGED_OOM);
    CHECK(string(&f, 0) == left && string(&f, 1) == left && !borrow(&f, 2) &&
        xr_signed_rc_read(&left->managed.references) == references);
    same_bill(&f, before);
    atomic_store(&f.audit.remaining, SIZE_MAX);
    OK(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, left));
    CHECK(string(&f, 0) != left && string(&f, 1) == left && !borrow(&f, 2));
    text_is(left, "a\0\xc3\xa9z" "a\0\xc3\xa9z", 10, 8);
    CHECK(string(&f, 0)->length == 20 && string(&f, 0)->runes == 16);
    fixture_done(&f);
}
static void binding_append_preflight(void) {
    Fixture f; fixture_init(&f, SIZE_MAX);
    XrtStringBacking *left = make(&f, 0, "a"), *suffix = make(&f, 3, "z");
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 0, &f.types, suffix), XRT_MANAGED_ARGUMENT);
    make(&f, 2, "occupied");
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_ARGUMENT);
    slot_is(&f, 2, "occupied"); OK(xrt_managed_slot_drop(f.frame, 2));
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(f.frame, 2, f.types.string, sizeof(XrtStringBacking) + 2, &pending));
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_ARGUMENT);
    CHECK(string(&f, 0) == left && !borrow(&f, 2)); same_bill(&f, before);
    OK(xrt_managed_slot_commit(f.frame, 2)); OK(xrt_managed_slot_drop(f.frame, 2));
    int32_t references = xr_signed_rc_read(&suffix->managed.references);
    atomic_store(&suffix->managed.references, INT32_MAX);
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_REFCOUNT);
    atomic_store(&suffix->managed.references, references);
    CHECK(string(&f, 0) == left && !borrow(&f, 2)); slot_is(&f, 0, "a");
    size_t length = suffix->length, capacity = suffix->capacity, runes = suffix->runes;
    suffix->length = SIZE_MAX - 1; suffix->capacity = SIZE_MAX; suffix->runes = 1;
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_OVERFLOW);
    suffix->length = length; suffix->capacity = capacity; suffix->runes = runes;
    CHECK(string(&f, 0) == left && !borrow(&f, 2)); slot_is(&f, 0, "a");
    /* The live fixture checks provider OOM separately from a closed budget. */
    xrt_billing_owner_close(f.billing);
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_CLOSED);
    CHECK(string(&f, 0) == left && !borrow(&f, 2)); slot_is(&f, 0, "a");
    fixture_done(&f);
}
typedef struct BindingAppendProbe {
    Fixture *target, *suffix_owner;
    XrtStringBacking *old, *suffix;
    size_t allocations, old_retirements, suffix_retirements;
    bool drained, fail;
} BindingAppendProbe;
static void binding_append_observe(void *context, void *memory, bool retiring) {
    BindingAppendProbe *probe = context;
    if (retiring && memory != probe->old && memory != probe->suffix) return;
    Fixture *f = probe->target;
    STATUS(xrt_managed_root_drain(f->root), XRT_MANAGED_ARGUMENT);
    const XrtManagedHeader *unchanged = NULL;
    STATUS(xrt_place_borrow(binding(f, 0), &unchanged), XRT_MANAGED_ARGUMENT); CHECK(!unchanged);
    STATUS(xrt_string_backing_append_place(binding(f, 0), f->frame, 2, &f->types, probe->suffix),
        XRT_MANAGED_ARGUMENT);
    CHECK(!borrow(f, 2));
    if (retiring) {
        if (probe->fail) {
            CHECK(memory == probe->suffix && string(f, 0) == probe->old); slot_is(f, 0, "a");
        } else { CHECK(string(f, 0) != probe->old); slot_is(f, 0, "az"); }
        if (memory == probe->old) ++probe->old_retirements;
        else ++probe->suffix_retirements;
    } else {
        CHECK(string(f, 0) == probe->old); slot_is(f, 0, "a");
        CHECK(xr_signed_rc_read(&probe->suffix->managed.references) == 2);
        ++probe->allocations;
        if (!probe->drained) {
            probe->drained = true;
            OK(xrt_managed_root_drain(probe->suffix_owner->root));
            probe->suffix_owner->frame = NULL;
        }
    }
}
static void binding_append_growth_and_callbacks(void) {
    Fixture f, other; fixture_init(&f, SIZE_MAX); fixture_init(&other, SIZE_MAX);
    XrtStringBacking *old = make(&f, 0, "a"), *suffix = make(&other, 0, "z");
    BindingAppendProbe probe = {.target=&f,.suffix_owner=&other,.old=old,.suffix=suffix,.fail=true};
    f.audit.observe = other.audit.observe = binding_append_observe;
    f.audit.observation = other.audit.observation = &probe;
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    atomic_store(&f.audit.remaining, 0);
    STATUS(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix), XRT_MANAGED_OOM);
    f.audit.observe = other.audit.observe = NULL;
    CHECK(probe.drained && probe.allocations && !probe.old_retirements && probe.suffix_retirements == 1);
    CHECK(string(&f, 0) == old && !borrow(&f, 2)); same_bill(&f, before);
    fixture_done(&other); fixture_init(&other, SIZE_MAX);
    suffix = make(&other, 0, "z");
    probe = (BindingAppendProbe){.target=&f,.suffix_owner=&other,.old=old,.suffix=suffix};
    f.audit.observe = other.audit.observe = binding_append_observe;
    f.audit.observation = other.audit.observation = &probe;
    atomic_store(&f.audit.remaining, SIZE_MAX);
    OK(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix));
    f.audit.observe = other.audit.observe = NULL;
    CHECK(probe.drained && probe.allocations && probe.old_retirements == 1 && probe.suffix_retirements == 1);
    fixture_done(&other);
    suffix = make(&f, 3, "z");
    size_t calls = atomic_load(&f.audit.calls);
    for (size_t i = 0; i < 4096; ++i) {
        OK(xrt_string_backing_append_place(binding(&f, 0), f.frame, 2, &f.types, suffix));
        CHECK(!borrow(&f, 2));
    }
    XrtStringBacking *value = string(&f, 0);
    CHECK(value->length == 4098 && value->runes == 4098 && atomic_load(&f.audit.calls) - calls < 16);
    CHECK(value->bytes[0] == 'a' && !value->bytes[4098]);
    for (size_t i = 1; i < value->length; ++i) CHECK(value->bytes[i] == 'z');
    fixture_done(&f);
}
int main(void) {
    read_unicode();
    derived_alias_chain();
    exclusive_append();
    formatted_values();
    pod_display_values();
    split_values();
    failures_and_refunds();
    independent_owners();
    owned_append_aliases();
    owned_append_preflight();
    append_capacity_window();
    owned_append_growth_and_callbacks();
    binding_append_aliases();
    binding_append_preflight();
    binding_append_growth_and_callbacks();
    puts("typed String operation kernel passed");
    return 0;
}
