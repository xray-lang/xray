/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_managed_status.c - Actual storage failures retain diagnostic identities
 */
#include "runtime/native/xnative_array_ops.h"
#include "runtime/native/xnative_status.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)

int main(void) {
    XrtBillingOwner *billing = NULL;
    XrtBackingTypes types = {0};
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_billing_owner_new(NULL, 8192, &billing));
    OK(xrt_backing_types_init(billing, &types));
    OK(xrt_managed_root_new(billing, &root));
    OK(xrt_managed_frame_enter(root, 3, &frame));
    XrtManagedStatus status = xrt_string_backing_create(frame, 0, &types, "\300\200", 2, 0);
    CHECK(status == XRT_MANAGED_BAD_UTF8 && xrt_managed_value_status(status) == XR_XIR_VALUE_BAD_UTF8);
    status = xrt_string_backing_create(frame, 0, &types, "x", SIZE_MAX, 0);
    CHECK(status == XRT_MANAGED_OVERFLOW && xrt_managed_value_status(status) == XR_XIR_VALUE_OVERFLOW);
    xrt_billing_owner_fail_after(billing, 0);
    status = xrt_string_backing_create(frame, 0, &types, "x", 1, 0);
    CHECK(status == XRT_MANAGED_OOM && xrt_managed_value_status(status) == XR_XIR_VALUE_OOM);
    CHECK((unsigned)xrt_managed_value_status(status) == 3);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    status = xrt_array_backing_u64_create(frame, 1, &types, NULL, 0, 8192);
    CHECK(status == XRT_MANAGED_LIMIT && xrt_managed_value_status(status) == XR_XIR_VALUE_MEMORY_LIMIT);
    CHECK((unsigned)xrt_managed_value_status(status) == 8);
    OK(xrt_array_backing_i64_create(frame, 1, &types, NULL, 0, 0));
    XrtManagedHeader *borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 1, &borrowed));
    XrtArrayItem item = {.kind=XRT_BACKING_ARRAY_U64, .value.u64=99};
    status = xrt_array_backing_get((const XrtArrayBacking *)borrowed, XRT_BACKING_ARRAY_I64, 0, &item);
    CHECK(status == XRT_MANAGED_BOUNDS && xrt_managed_value_status(status) == XR_XIR_VALUE_BOUNDS);
    CHECK(item.kind == XRT_BACKING_ARRAY_U64 && item.value.u64 == 99);
    OK(xrt_string_backing_create(frame, 0, &types, "kept", 4, 0));
    borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &borrowed));
    atomic_store(&borrowed->references, INT32_MAX);
    status = xrt_backing_slot_copy(frame, 2, borrowed);
    CHECK(status == XRT_MANAGED_REFCOUNT && xrt_managed_value_status(status) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&borrowed->references, 1);
    xrt_billing_owner_close(billing);
    status = xrt_string_backing_create(frame, 0, &types, "new", 3, 0);
    CHECK(status == XRT_MANAGED_CLOSED && xrt_managed_value_status(status) == XR_XIR_VALUE_BAD_ARGUMENT);
    XrtManagedHeader *unchanged = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &unchanged));
    CHECK(unchanged == borrowed);
    CHECK(xrt_managed_value_status(XRT_MANAGED_OK) == XR_XIR_VALUE_OK);
    CHECK(xrt_managed_value_status((XrtManagedStatus)99) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_managed_root_destroy(&root));
    xrt_backing_types_drop(&types);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
    puts("Real UTF-8, allocation, quota, overflow, bounds and ownership failures keep diagnostic identities");
    return 0;
}
