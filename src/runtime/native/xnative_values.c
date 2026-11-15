/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_values.c - Stable fixed-value owners across synchronous recovery
 *
 * KEY CONCEPT:
 *   Publish each context owner before the next fallible operation. The enclosing
 *   execution drains the same storage on normal return and longjmp recovery.
 */
#include "xnative_values.h"
#include "xray_native_values.h"
#include "xnative_string_backing.h"
#include "xnative_status.h"
#include "base/xchecks.h"
#include <string.h>

struct XrtNativeValues {
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    XrtBackingTypes backings;
};

XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source_id) {
    XR_CHECK(status != XRT_MANAGED_OK, "managed failure preserves a real failing kernel result");
    xrt_value_failure(xrt_managed_value_status(status), source_id);
}

static struct XrtNativeValues *native_values(uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    XR_CHECK(!owner->draining, "cleanup callbacks cannot reenter the retiring execution");
    if (!owner->managed) {
        XrtBillingOwner *billing = NULL;
        size_t limit = SIZE_MAX;
        if (owner->byte_limit) {
#if SIZE_MAX < UINT64_MAX
            if (owner->byte_limit > SIZE_MAX) xrt_native_managed_failure(XRT_MANAGED_LIMIT, source_id);
#endif
            limit = (size_t)owner->byte_limit;
        }
        XrtManagedStatus status = xrt_billing_owner_new(NULL, limit, &billing);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
        void *memory = NULL;
        status = xrt_managed_metadata_allocate(billing, sizeof(struct XrtNativeValues), &memory);
        if (status != XRT_MANAGED_OK) {
            xrt_billing_owner_drop(billing);
            xrt_native_managed_failure(status, source_id);
        }
        memset(memory, 0, sizeof(struct XrtNativeValues));
        struct XrtNativeValues *values = memory;
        values->billing = billing;
        owner->managed = values;
    }
    struct XrtNativeValues *values = owner->managed;
    if (!values->root) {
        XrtManagedStatus status = xrt_managed_root_new(values->billing, &values->root);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    }
    return values;
}

XRAY_API XrtManagedRoot *xrt_native_managed_root(uint32_t source_id) {
    return native_values(source_id)->root;
}

XRAY_API void xrt_native_string_create(XrtManagedFrame *frame, size_t slot,
    const char *bytes, size_t length, uint32_t source_id) {
    struct XrtNativeValues *values = native_values(source_id);
    XrtManagedStatus status = XRT_MANAGED_OK;
    if (!values->backings.string) status = xrt_backing_types_init(values->billing, &values->backings);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_create(frame, slot, &values->backings, bytes, length, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id) {
    struct XrtNativeValues *values = native_values(source_id);
    XrtManagedStatus status = XRT_MANAGED_OK;
    if (!values->backings.string) status = xrt_backing_types_init(values->billing, &values->backings);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_concat_create(frame, slot, &values->backings,
            (const XrtStringBacking *)left, (const XrtStringBacking *)right, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XR_FUNC void xrt_native_values_drain(XrtValueRoot *owner) {
    struct XrtNativeValues *values = owner->managed;
    if (!values) return;
    XrtManagedStatus status = xrt_managed_root_destroy(&values->root);
    XR_CHECK(status == XRT_MANAGED_OK, "execution retirement runs outside every managed mutation");
    xrt_backing_types_drop(&values->backings);
    XrtBillingOwner *billing = values->billing;
    owner->managed = NULL;
    xrt_managed_metadata_free(billing, values, sizeof(*values));
    xrt_billing_owner_drop(billing);
}
