/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_class_identity.c - Direction of a Class-identity observation
 *
 * KEY CONCEPT:
 *   A Class-identity union value is one nominal handle. `is` must ask whether
 *   the value's actual class equals the target or derives from it, walking UP
 *   from the value's class. Walking DOWN from the target instead would answer
 *   the reverse question and report an ancestor as an instance of its child.
 *   These fixtures build the class mirrors directly through the same receiving
 *   image and construction path generated C uses, with no source compilation.
 */
#include "runtime/native/xnative_value.h"
#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_place.h"
#include "runtime/native/xnative_value_image.h"
#include "runtime/native/xnative_class_array.h"
#include "runtime/native/xnative_host_nullable.h"
#include "runtime/native/xnative_host_stat.h"
#include "runtime/native/xnative_string_backing.h"
#include "base/xmalloc.h"
#include "shared/xnative_declaration.h"
#include "xray_native_values.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)

enum { ROW_I64 = 1, ROW_BASE_PAYLOAD = 2, ROW_BASE = 3,
    ROW_DERIVED_PAYLOAD = 4, ROW_DERIVED = 5,
    ROW_GRAND_PAYLOAD = 6, ROW_GRAND = 7 };

/* One field per payload keeps the three mirrors distinguishable only by their
 * nominal Class row and parent link, exactly like the compiler's layout. */
typedef struct Payload { int64_t fd; } Payload;

static const char field_names[] = "fd";
static const size_t class_owned[] = {0};
static const XrtValueImageSlot slots[] = {
    {ROW_I64, 0, 0, 0, 2},
    {ROW_I64, 0, 0, 0, 2},
    {ROW_I64, 0, 0, 0, 2},
};
static const XrtValueImageType rows[] = {
    /* 1 */ {.kind = XRT_VALUE_I64, .size = 8, .alignment = 8},
    /* 2 */ {.kind = XRT_VALUE_RECORD, .size = 8, .alignment = 8, .first_field = 0, .field_count = 1},
    /* 3 */ {.kind = XRT_VALUE_CLASS, .size = sizeof(void *), .alignment = alignof(void *),
        .first_owned = 0, .owned_count = 1, .element = ROW_BASE_PAYLOAD},
    /* 4 */ {.kind = XRT_VALUE_RECORD, .size = 8, .alignment = 8, .first_field = 1, .field_count = 1},
    /* 5 */ {.kind = XRT_VALUE_CLASS, .size = sizeof(void *), .alignment = alignof(void *),
        .first_owned = 0, .owned_count = 1, .element = ROW_DERIVED_PAYLOAD, .class_parent = ROW_BASE},
    /* 6 */ {.kind = XRT_VALUE_RECORD, .size = 8, .alignment = 8, .first_field = 2, .field_count = 1},
    /* 7 */ {.kind = XRT_VALUE_CLASS, .size = sizeof(void *), .alignment = alignof(void *),
        .first_owned = 0, .owned_count = 1, .element = ROW_GRAND_PAYLOAD, .class_parent = ROW_DERIVED},
};
static const XrtValueImage image = {
    .types = rows, .type_count = sizeof(rows) / sizeof(rows[0]),
    .slots = slots, .slot_count = sizeof(slots) / sizeof(slots[0]),
    .owned_offsets = class_owned, .owned_count = sizeof(class_owned) / sizeof(class_owned[0]),
    .names = field_names, .name_bytes = sizeof(field_names) - 1,
};

static XrtManagedHeader *instance(XrtManagedFrame *frame, size_t slot, uint32_t type, int64_t fd) {
    Payload payload = {.fd = fd};
    xrt_native_class_create(frame, slot, &image, type, &payload, 1);
    XrtManagedHeader *owner = NULL;
    OK(xrt_managed_slot_borrow(frame, slot, &owner));
    CHECK(owner && owner->ops);
    return owner;
}

static void file_stat_payload_facts(void) {
    XrtBillingOwner *billing = NULL;
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrtValuePlan *integer = NULL, *boolean = NULL, *nullable = NULL, *record = NULL;
    const XrtValuePlanSpec i64 = {.kind=XRT_VALUE_I64, .size=8, .alignment=8};
    const XrtValuePlanSpec flag = {.kind=XRT_VALUE_BOOL, .size=1, .alignment=1};
    OK(xrt_value_plan_new(billing, &i64, &integer));
    OK(xrt_value_plan_new(billing, &flag, &boolean));
    const XrtValuePlanSpec optional_i64 = {.kind=XRT_VALUE_NULLABLE, .size=16, .alignment=8,
        .nullable={.tag_offset=0, .payload_offset=8, .element=integer}};
    OK(xrt_value_plan_new(billing, &optional_i64, &nullable));
    XrtValueSlotSpec fields[] = {
        {integer, 0, "size", 4, 0}, {integer, 8, "mode", 4, 0}, {integer, 16, "mtime", 5, 0},
        {integer, 24, "atime", 5, 0}, {integer, 32, "ctime", 5, 0}, {nullable, 40, "uid", 3, 0},
        {nullable, 56, "gid", 3, 0}, {boolean, 72, "isFile", 6, 0}, {boolean, 73, "isDir", 5, 0},
        {boolean, 74, "isSymlink", 9, 0},
    };
    const XrtValuePlanSpec type = {.kind=XRT_VALUE_RECORD, .size=80, .alignment=8,
        .fields=fields, .field_count=sizeof(fields) / sizeof(*fields)};
    OK(xrt_value_plan_new(billing, &type, &record));
    XrtValuePlanSpec received;
    OK(xrt_value_plan_view(record, &received));
    CHECK(xrt_host_stat_row_matches(&received));
    const size_t owner_offset = 0;
    XrtValuePlanSpec stat_class = {.kind=XRT_VALUE_CLASS, .size=sizeof(XrtManagedHeader *),
        .alignment=alignof(XrtManagedHeader *), .owned_offsets=&owner_offset, .owned_count=1,
        .element=record, .class_storage=XR_NATIVE_DECLARATION_FILE_STAT};
    XrtValuePlan *stat_plan = NULL;
    OK(xrt_value_plan_new(billing, &stat_class, &stat_plan));

    fields[5].type = boolean;
    CHECK(!xrt_host_stat_row_matches(&type));
    fields[5].type = nullable;
    fields[0].flags = XRT_VALUE_FIELD_MUTABLE;
    CHECK(!xrt_host_stat_row_matches(&type));
    fields[0].flags = 0;
    fields[6].offset = 57;
    CHECK(!xrt_host_stat_row_matches(&type));
    CHECK(xrt_host_stat_row_matches(&received));
    fields[6].offset = 56;
    fields[6].name = "bad";
    XrtValuePlan *wrong_name = NULL, *rejected = NULL;
    OK(xrt_value_plan_new(billing, &type, &wrong_name));
    stat_class.element = wrong_name;
    CHECK(xrt_value_plan_new(billing, &stat_class, &rejected) == XRT_MANAGED_ARGUMENT && !rejected);
    xrt_value_plan_drop(wrong_name);

    const XrOsFileMetadata metadata = {.present=true, .size=42, .mode=0644, .mtime=-30, .atime=40, .ctime=50,
        .uid={true, 0}, .gid={false, 13}, .is_file=true, .is_dir=false, .is_symlink=true};
    XrtHostStatRow row;
    memset(&row, 0xa5, sizeof(row));
    xrt_host_stat_row(&metadata, &row);
    CHECK(row.size == 42 && row.mode == 0644 && row.mtime == -30 && row.atime == 40 && row.ctime == 50);
    CHECK(row.uid.present == 1 && row.uid.value == 0 && row.gid.present == 0 && row.gid.value == 0);
    CHECK(row.is_file == 1 && row.is_dir == 0 && row.is_symlink == 1);
    unsigned char expected[80] = {0};
    const int64_t required[] = {42, 0644, -30, 40, 50};
    memcpy(expected, required, sizeof(required));
    expected[40] = expected[72] = expected[74] = 1;
    CHECK(sizeof(row) == sizeof(expected) && !memcmp(&row, expected, sizeof(expected)));

    XrtManagedOps *ops = NULL;
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_class_backing_bind(billing, stat_plan, &ops));
    OK(xrt_managed_root_new(billing, &root));
    OK(xrt_managed_frame_enter(root, 1, &frame));
    CHECK(xrt_class_create(frame, 0, ops, &row) == XRT_MANAGED_ARGUMENT);
    OK(xrt_host_stat_class_create(frame, 0, (XrtBoundClass){stat_plan, ops}, &row));
    XrtManagedHeader *object = NULL;
    const XrtValuePlan *field_type = NULL;
    const void *field = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &object));
    OK(xrt_class_field_borrow(object, stat_plan, 0, &field_type, &field));
    CHECK(field_type == integer && *(const int64_t *)field == 42);
    const int64_t replacement = 1;
    CHECK(xrt_class_scalar_field_write(object, stat_plan, 0, &replacement, sizeof(replacement)) == XRT_MANAGED_ARGUMENT);
    OK(xrt_managed_root_destroy(&root));
    xrt_managed_ops_drop(ops);
    xrt_value_plan_drop(stat_plan);

    xrt_value_plan_drop(record);
    xrt_value_plan_drop(nullable);
    xrt_value_plan_drop(integer);
    xrt_value_plan_drop(boolean);
    xrt_billing_owner_drop(billing);
}

typedef struct StringClassRow {
    XrtManagedHeader *plain;
    XrtHostNullableOwner optional;
    XrtManagedHeader *immutable;
} StringClassRow;
typedef struct StringClassBacking {
    XrtManagedHeader header;
    StringClassRow row;
    bool guards[3];
} StringClassBacking;

typedef struct StringRetirement {
    const void *watched;
    const bool *field_guard;
    XrtManagedHeader *const *published;
    const XrtManagedHeader *replacement;
    XrtManagedFrame *frame;
    size_t slot, observed, leases;
} StringRetirement;

static bool string_allocator_retain(void *context) {
    ++((StringRetirement *)context)->leases;
    return true;
}
static void string_allocator_drop(void *context) {
    StringRetirement *audit = context;
    CHECK(audit->leases == 1);
    --audit->leases;
}
static void *string_allocator_alloc(void *context, size_t bytes) {
    (void)context;
    return xr_malloc(bytes);
}
static void string_allocator_free(void *context, void *memory, size_t bytes) {
    (void)bytes;
    StringRetirement *audit = context;
    if (memory == audit->watched) {
        audit->watched = NULL;
        CHECK(!*audit->field_guard && *audit->published == audit->replacement);
        if (audit->frame) CHECK(xrt_managed_slot_drop(audit->frame, audit->slot) == XRT_MANAGED_ARGUMENT);
        ++audit->observed;
    }
    xr_free(memory);
}

static XrtValuePlan *string_field_plan(XrtBillingOwner *billing, XrtValuePlanSpec spec) {
    XrtValuePlan *plan = NULL;
    OK(xrt_value_plan_new(billing, &spec, &plan));
    return plan;
}
static XrtManagedHeader *read_owner(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *owner = NULL;
    OK(xrt_managed_slot_borrow(frame, slot, &owner));
    return owner;
}
static void check_text(const XrtManagedHeader *owner, const char *expected) {
    CHECK(xrt_backing_string_matches(owner));
    const XrtStringBacking *text = (const XrtStringBacking *)owner;
    CHECK(text->length == strlen(expected) && !memcmp(text->bytes, expected, text->length));
}

enum { INSTANCE_SLOT, SCRATCH_SLOT, OPTIONAL_SLOT, STRING_SLOT_COUNT };

static void string_field_refusals(const XrtClassFieldAccess *field, XrtManagedFrame *frame,
    XrtHostNullableOwner *result, StringClassBacking *backing, XrtManagedHeader *first, XrtManagedHeader *second) {
    const XrtManagedStatus bad = XRT_MANAGED_ARGUMENT;
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, SIZE_MAX) == bad);
    CHECK(xrt_class_string_field_copy(field, frame, SCRATCH_SLOT, 0) == bad);
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, 1) == bad);
    CHECK(xrt_class_string_field_replace(field, first, 2, true) == bad);
    CHECK(xrt_class_string_field_replace(field, first, 0, true) == bad);
    CHECK(xrt_class_string_field_replace(field, NULL, 1, true) == bad);
    CHECK(xrt_class_string_field_replace(field, first, 1, false) == bad);
    CHECK(xrt_class_string_field_replace(field, &backing->header, 1, true) == bad);
    XrtManagedSlotLocation guard = {.busy=&backing->guards[1]};
    OK(xrt_managed_domain_enter(guard));
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, 0) == bad);
    CHECK(xrt_class_string_field_replace(field, first, 1, true) == bad);
    CHECK(xrt_managed_domain_held(guard));
    xrt_managed_domain_leave(guard);

    result->present = 2;
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, 0) == bad);
    CHECK(result->present == 2 && result->owner == first);
    result->present = 1;
    backing->row.optional.present = 0;
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, 0) == bad);
    CHECK(xrt_class_string_field_replace(field, first, 1, true) == bad);
    CHECK(!backing->row.optional.present && backing->row.optional.owner == second);
    backing->row.optional.present = 1;

    const int32_t first_references = xr_signed_rc_read(&first->references);
    atomic_store(&first->references, INT32_MAX);
    CHECK(xrt_class_string_field_replace(field, first, 1, true) == XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&first->references) == INT32_MAX);
    atomic_store(&first->references, first_references);
    const int32_t second_references = xr_signed_rc_read(&second->references);
    atomic_store(&second->references, INT32_MAX);
    CHECK(xrt_class_string_field_copy(field, frame, OPTIONAL_SLOT, 0) == XRT_MANAGED_REFCOUNT);
    CHECK(xrt_class_string_field_replace(field, second, 1, true) == XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&second->references) == INT32_MAX);
    atomic_store(&second->references, second_references);
    CHECK(result->present == 1 && result->owner == first && backing->row.optional.present == 1 &&
        backing->row.optional.owner == second && !xrt_managed_domain_held(guard));
}

static void string_field_transactions(void) {
    StringRetirement audit = {0};
    XrtManagedAllocator allocator = {&audit, string_allocator_alloc, string_allocator_free,
        {&audit, string_allocator_retain, string_allocator_drop}};
    XrtBillingOwner *billing = NULL;
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &billing));
    XrtBillingStats baseline = xrt_billing_owner_stats(billing);
    const size_t pointer = 0, optional_owner = offsetof(XrtHostNullableOwner, owner);
    XrtValuePlan *text = string_field_plan(billing, (XrtValuePlanSpec){.kind=XRT_VALUE_STRING,
        .size=sizeof(void *), .alignment=alignof(void *), .owned_offsets=&pointer, .owned_count=1});
    XrtValuePlan *nullable = string_field_plan(billing, (XrtValuePlanSpec){.kind=XRT_VALUE_NULLABLE,
        .size=sizeof(XrtHostNullableOwner), .alignment=alignof(XrtHostNullableOwner), .owned_offsets=&optional_owner,
        .owned_count=1, .nullable={.element=text,.payload_offset=optional_owner}});
    const size_t owners[] = {offsetof(StringClassRow, plain), offsetof(StringClassRow, optional) + optional_owner,
        offsetof(StringClassRow, immutable)};
    XrtValueSlotSpec fields[] = {{text, offsetof(StringClassRow, plain), "plain", 5, XRT_VALUE_FIELD_MUTABLE},
        {nullable, offsetof(StringClassRow, optional), "optional", 8, XRT_VALUE_FIELD_MUTABLE},
        {text, offsetof(StringClassRow, immutable), "immutable", 9, 0}};
    XrtValuePlan *record = string_field_plan(billing, (XrtValuePlanSpec){.kind=XRT_VALUE_RECORD,
        .size=sizeof(StringClassRow), .alignment=alignof(StringClassRow), .owned_offsets=owners,
        .owned_count=3, .fields=fields, .field_count=3});
    const XrtValuePlanSpec type = {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=alignof(void *),
        .owned_offsets=&pointer,.owned_count=1,.element=record};
    XrtValuePlan *class_plan = string_field_plan(billing, type);
    XrtValuePlan *other_plan = string_field_plan(billing, type);
    XrtManagedOps *ops = NULL;
    OK(xrt_class_backing_bind(billing, class_plan, &ops));
    XrtBackingTypes types = {0};
    OK(xrt_backing_types_init(billing, &types));
    XrtManagedRoot *root = NULL, *producer = NULL;
    XrtManagedFrame *frame = NULL, *input = NULL;
    OK(xrt_managed_root_new(billing, &root));
    OK(xrt_managed_root_new(billing, &producer));
    OK(xrt_managed_frame_enter(producer, 2, &input));
    OK(xrt_string_backing_create(input, 0, &types, "first", 5, 0));
    OK(xrt_string_backing_create(input, 1, &types, "second", 6, 0));
    XrtManagedHeader *first = read_owner(input, 0), *second = read_owner(input, 1);
    const size_t frame_owners[] = {SIZE_MAX, SIZE_MAX, optional_owner};
    const XrtManagedFrameSpec layout = {STRING_SLOT_COUNT, sizeof(XrtHostNullableOwner),
        alignof(XrtHostNullableOwner), frame_owners};
    void *bytes = NULL;
    OK(xrt_managed_frame_enter_payload(root, &layout, &frame, &bytes));
    XrtHostNullableOwner *result = bytes;
    StringClassRow initial = {.plain=first,.immutable=first};
    const XrtHostNullableOwner invalid[] = {{2,first}, {0,first}, {1,NULL}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        initial.optional = invalid[i];
        CHECK(xrt_class_create(frame, INSTANCE_SLOT, ops, &initial) == XRT_MANAGED_ARGUMENT);
        CHECK(!read_owner(frame, INSTANCE_SLOT));
    }
    initial.optional = (XrtHostNullableOwner){0};
    OK(xrt_class_create(frame, INSTANCE_SLOT, ops, &initial));
    StringClassBacking *backing = (StringClassBacking *)read_owner(frame, INSTANCE_SLOT);
    XrtClassFieldAccess plain = {&backing->header,class_plan,0}, optional = {&backing->header,class_plan,1};
    XrtClassFieldAccess immutable = {&backing->header,class_plan,2}, foreign = {&backing->header,other_plan,1};
    XrtClassFieldAccess missing = {&backing->header,class_plan,3};
    CHECK(xrt_class_string_field_copy(&foreign, frame, OPTIONAL_SLOT, 0) == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_class_string_field_replace(&immutable, first, 1, false) == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_class_string_field_copy(&missing, frame, OPTIONAL_SLOT, 0) == XRT_MANAGED_BOUNDS);
    const XrtValuePlan *borrow_type = NULL;
    const void *borrow = NULL;
    CHECK(xrt_class_field_borrow(&backing->header, class_plan, 0, &borrow_type, &borrow) == XRT_MANAGED_ARGUMENT);
    OK(xrt_class_field_borrow(&backing->header, class_plan, 2, &borrow_type, &borrow));
    CHECK(borrow_type == text && *(XrtManagedHeader *const *)borrow == first);

    initial.optional = (XrtHostNullableOwner){1,&backing->header};
    CHECK(xrt_class_create(frame, SCRATCH_SLOT, ops, &initial) == XRT_MANAGED_ARGUMENT);
    initial.optional.owner = first;
    initial.immutable = NULL;
    CHECK(xrt_class_create(frame, SCRATCH_SLOT, ops, &initial) == XRT_MANAGED_ARGUMENT);
    initial.immutable = first;
    OK(xrt_class_create(frame, SCRATCH_SLOT, ops, &initial));
    const XrtClassFieldAccess initialized = {read_owner(frame, SCRATCH_SLOT),class_plan,1};
    OK(xrt_class_string_field_copy(&initialized, frame, OPTIONAL_SLOT, 0));
    OK(xrt_managed_slot_drop(frame, SCRATCH_SLOT));
    CHECK(result->present == 1 && result->owner == first);
    OK(xrt_class_string_field_copy(&optional, frame, OPTIONAL_SLOT, 0));
    CHECK(!result->present && !result->owner);
    OK(xrt_class_string_field_replace(&optional, first, 1, true));
    OK(xrt_class_string_field_copy(&optional, frame, OPTIONAL_SLOT, 0));
    CHECK(result->present == 1 && result->owner == first);
    OK(xrt_class_string_field_replace(&optional, second, 1, true));
    check_text(result->owner, "first");
    string_field_refusals(&optional, frame, result, backing, first, second);
    const int32_t references = xr_signed_rc_read(&second->references);
    OK(xrt_class_string_field_replace(&optional, second, 1, true));
    CHECK(xr_signed_rc_read(&second->references) == references);
    OK(xrt_class_string_field_copy(&optional, frame, OPTIONAL_SLOT, 0));
    CHECK(result->present == 1 && result->owner == second);
    OK(xrt_class_string_field_replace(&optional, NULL, 0, true));
    check_text(result->owner, "second");
    OK(xrt_class_string_field_copy(&optional, frame, OPTIONAL_SLOT, 0));
    CHECK(!result->present && !result->owner);

    OK(xrt_string_backing_create(frame, SCRATCH_SLOT, &types, "retired", 7, 0));
    XrtManagedHeader *retired = read_owner(frame, SCRATCH_SLOT);
    OK(xrt_class_string_field_replace(&plain, retired, 1, false));
    OK(xrt_managed_slot_drop(frame, SCRATCH_SLOT));
    audit.watched = retired; audit.field_guard = &backing->guards[0];
    audit.published = &backing->row.plain; audit.replacement = second;
    OK(xrt_class_string_field_replace(&plain, second, 1, false));
    CHECK(audit.observed == 1 && !audit.watched);
    OK(xrt_managed_root_destroy(&producer));
    XrtManagedSlotLocation destination;
    OK(xrt_managed_slot_location(frame, INSTANCE_SLOT, &destination));
    audit.watched = backing; audit.published = destination.owner; audit.frame = frame; audit.slot = INSTANCE_SLOT;
    OK(xrt_class_string_field_copy(&plain, frame, INSTANCE_SLOT, SIZE_MAX));
    CHECK(audit.observed == 2 && !audit.watched);
    check_text(read_owner(frame, INSTANCE_SLOT), "second");

    OK(xrt_managed_root_destroy(&root));
    xrt_backing_types_drop(&types);
    xrt_managed_ops_drop(ops);
    xrt_value_plan_drop(other_plan);
    xrt_value_plan_drop(class_plan);
    xrt_value_plan_drop(record);
    xrt_value_plan_drop(nullable);
    xrt_value_plan_drop(text);
    XrtBillingStats remaining = xrt_billing_owner_stats(billing);
    CHECK(remaining.bytes == baseline.bytes && remaining.blocks == baseline.blocks);
    xrt_billing_owner_drop(billing);
    CHECK(!audit.leases);
    puts("class String fields: guarded snapshots, failure-atomic replacement and receiver retirement verified");
}

static XrtManagedHeader *slot_owner(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *owner = NULL;
    OK(xrt_managed_slot_borrow(frame, slot, &owner));
    return owner;
}

static void check_inline_class_row(const XrtArrayBacking *array, const XrtValuePlan *element,
    int64_t index, const XrtManagedHeader *expected) {
    const void *row = NULL;
    OK(xrt_array_backing_inline_at(array, element, index, &row));
    XrtManagedHeader *actual = NULL;
    memcpy(&actual, row, sizeof(actual));
    CHECK(actual == expected);
}

static void inline_class_append(void) {
    enum { FIRST, SECOND, OTHER, ARRAY, STAGING, ALIAS, SLOT_COUNT };
    const size_t limit = 65536;
    XrtBillingOwner *billing = NULL;
    XrtValueImagePlans *plans = NULL;
    XrtManagedOps *array_ops = NULL;
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    XrtBoundClass base = {0}, other = {0};
    OK(xrt_billing_owner_new(NULL, limit, &billing));
    const XrtBillingStats empty = xrt_billing_owner_stats(billing);
    OK(xrt_value_image_bind(billing, &image, (XrtManagedLease){0}, &plans));
    OK(xrt_value_image_class(plans, ROW_BASE, &base));
    OK(xrt_value_image_class(plans, ROW_DERIVED, &other));
    OK(xrt_array_backing_inline_bind(billing, base.plan, &array_ops));
    OK(xrt_managed_root_new(billing, &root));
    OK(xrt_managed_frame_enter(root, SLOT_COUNT, &frame));

    const Payload one = {1}, two = {2}, three = {3};
    OK(xrt_class_create(frame, FIRST, base.ops, &one));
    OK(xrt_class_create(frame, SECOND, base.ops, &two));
    OK(xrt_class_create(frame, OTHER, other.ops, &three));
    XrtManagedHeader *first = slot_owner(frame, FIRST);
    XrtManagedHeader *second = slot_owner(frame, SECOND);
    XrtManagedHeader *wrong = slot_owner(frame, OTHER);
    XrtManagedHeader *rows_to_copy[] = {first, second};
    OK(xrt_array_backing_inline_create(frame, ARRAY, array_ops, rows_to_copy, 2, 4));
    XrtArrayBacking *old = (XrtArrayBacking *)slot_owner(frame, ARRAY);
    XrtPlace binding = {.frame=frame, .slot=ARRAY, .nulls=XRT_PLACE_NONNULL};
    CHECK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, wrong)
        == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_array_backing_inline_class_append(binding, ARRAY, array_ops, base.plan, first)
        != XRT_MANAGED_OK);
    CHECK(old->length == 2 && slot_owner(frame, STAGING) == NULL);

    XrtPlaceReplacement occupied = {0};
    OK(xrt_place_replacement_begin(binding, frame, STAGING, &occupied));
    CHECK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, first)
        != XRT_MANAGED_OK);
    OK(xrt_place_replacement_end(&occupied));
    OK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, first));
    CHECK(slot_owner(frame, ARRAY) == &old->managed && old->length == 3 && old->capacity == 4);
    check_inline_class_row(old, base.plan, 2, first);
    CHECK(xrt_managed_retain(&old->managed));
    XrtManagedHeader *snapshot = &old->managed;
    OK(xrt_managed_slot_adopt(frame, ALIAS, &snapshot));
    CHECK(snapshot == NULL);

    const XrtBillingStats before = xrt_billing_owner_stats(billing);
    int32_t first_count = xr_signed_rc_read(&first->references);
    int32_t second_count = xr_signed_rc_read(&second->references);
    atomic_store_explicit(&second->references, INT32_MAX, memory_order_relaxed);
    CHECK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, first)
        == XRT_MANAGED_REFCOUNT);
    atomic_store_explicit(&second->references, second_count, memory_order_relaxed);
    CHECK(xr_signed_rc_read(&first->references) == first_count);
    CHECK(xrt_billing_owner_stats(billing).bytes == before.bytes &&
        xrt_billing_owner_stats(billing).blocks == before.blocks);
    CHECK(slot_owner(frame, ARRAY) == &old->managed && slot_owner(frame, ALIAS) == &old->managed &&
        old->length == 3 && slot_owner(frame, STAGING) == NULL);
    check_inline_class_row(old, base.plan, 1, second);

    void *filler = NULL;
    CHECK(before.bytes < limit);
    OK(xrt_managed_metadata_allocate(billing, limit - before.bytes, &filler));
    CHECK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, second)
        == XRT_MANAGED_LIMIT);
    CHECK(xrt_billing_owner_stats(billing).bytes == limit);
    xrt_managed_metadata_free(billing, filler, limit - before.bytes);
    CHECK(slot_owner(frame, ARRAY) == &old->managed && old->length == 3 && slot_owner(frame, STAGING) == NULL);

    OK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, second));
    XrtArrayBacking *copy = (XrtArrayBacking *)slot_owner(frame, ARRAY);
    CHECK(copy != old && copy->length == 4 && old->length == 3);
    check_inline_class_row(copy, base.plan, 3, second);
    OK(xrt_array_backing_inline_class_append(binding, STAGING, array_ops, base.plan, first));
    XrtArrayBacking *grown = (XrtArrayBacking *)slot_owner(frame, ARRAY);
    CHECK(grown->length == 5 && grown->capacity == 8 && old->length == 3);
    check_inline_class_row(grown, base.plan, 4, first);

    OK(xrt_managed_root_destroy(&root));
    xrt_managed_ops_drop(array_ops);
    xrt_value_image_drop(&plans);
    const XrtBillingStats remaining = xrt_billing_owner_stats(billing);
    CHECK(remaining.bytes == empty.bytes && remaining.blocks == empty.blocks);
    xrt_billing_owner_drop(billing);
}

int main(void) {
    inline_class_append();
    file_stat_payload_facts();
    string_field_transactions();
    XrtValueRoot owner = {0};
    XrtValueBinding previous = xrt_value_root_bind((XrtValueBinding){&owner, 1});
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtManagedFrame *frame = NULL;
    CHECK(root);
    OK(xrt_managed_frame_enter(root, 3, &frame));

    /* A Base instance is exactly Base and is NOT an instance of its children.
     * This is the assertion that fails when the ancestor walk is reversed. */
    XrtManagedHeader *base = instance(frame, 0, ROW_BASE, 11);
    CHECK(xrt_native_class_is(base, &image, ROW_BASE, 1));
    CHECK(!xrt_native_class_is(base, &image, ROW_DERIVED, 1));
    CHECK(!xrt_native_class_is(base, &image, ROW_GRAND, 1));

    /* A Derived instance is Derived, its direct parent Base, and no grandchild. */
    XrtManagedHeader *derived = instance(frame, 1, ROW_DERIVED, 22);
    CHECK(xrt_native_class_is(derived, &image, ROW_BASE, 1));
    CHECK(xrt_native_class_is(derived, &image, ROW_DERIVED, 1));
    CHECK(!xrt_native_class_is(derived, &image, ROW_GRAND, 1));

    /* A Grand instance reaches both ancestors across the multi-level chain. */
    XrtManagedHeader *grand = instance(frame, 2, ROW_GRAND, 33);
    CHECK(xrt_native_class_is(grand, &image, ROW_BASE, 1));
    CHECK(xrt_native_class_is(grand, &image, ROW_DERIVED, 1));
    CHECK(xrt_native_class_is(grand, &image, ROW_GRAND, 1));

    OK(xrt_managed_frame_leave(root, frame));
    xrt_value_root_drain(&owner);
    (void)xrt_value_root_bind(previous);
    printf("class identity: base/derived/grand direction and chain verified\n");
    return 0;
}
