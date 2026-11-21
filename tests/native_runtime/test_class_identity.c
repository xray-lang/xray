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
#include "xray_native_values.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
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
