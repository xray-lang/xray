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
#include "runtime/native/xnative_value_image.h"
#include "runtime/native/xnative_host_stat.h"
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

    fields[5].type = boolean;
    CHECK(!xrt_host_stat_row_matches(&type));
    fields[5].type = nullable;
    fields[0].flags = XRT_VALUE_FIELD_MUTABLE;
    CHECK(!xrt_host_stat_row_matches(&type));
    fields[0].flags = 0;
    fields[6].offset = 57;
    CHECK(!xrt_host_stat_row_matches(&type));
    CHECK(xrt_host_stat_row_matches(&received));

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

    xrt_value_plan_drop(record);
    xrt_value_plan_drop(nullable);
    xrt_value_plan_drop(integer);
    xrt_value_plan_drop(boolean);
    xrt_billing_owner_drop(billing);
}

int main(void) {
    file_stat_payload_facts();
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
