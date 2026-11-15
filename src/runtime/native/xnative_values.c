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
#include "xnative_array_ops.h"
#include "xnative_status.h"
#include "base/xchecks.h"
#include <string.h>

struct XrtNativeValues {
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    XrtBackingTypes backings;
};
static XR_THREAD_LOCAL XrtValueBinding value_binding;
XR_FUNC XrtValueBinding xrt_value_root_bind(XrtValueBinding binding) {
    XrtValueBinding previous = value_binding;
    value_binding = binding;
    return previous;
}
XR_FUNC XrtValueRoot *xrt_value_root_active(void) { return value_binding.root; }
XR_FUNC XrtValueRoot *xrt_value_root_current(uint32_t source_id) {
    XR_CHECK(value_binding.root && source_id <= value_binding.sources,
        "value operations require an active execution and its source id");
    return value_binding.root;
}
XR_FUNC void xrt_value_root_drain(XrtValueRoot *owner) {
    if (!owner || owner->draining) return;
    uint64_t limit = owner->byte_limit;
    owner->draining = true;
    xrt_native_values_drain(owner);
    *owner = (XrtValueRoot){.byte_limit=limit};
}

XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source_id) {
    XR_CHECK(status != XRT_MANAGED_OK, "managed failure preserves a real failing kernel result");
    xrt_value_failure(xrt_managed_value_status(status), source_id);
}

XR_FUNC XrtManagedStatus xrt_native_values_prepare(XrtValueRoot *owner, XrtBillingOwner **output) {
    if (!owner || owner->draining || !output || *output) return XRT_MANAGED_ARGUMENT;
    if (!owner->managed) {
        XrtBillingOwner *billing = NULL;
        size_t limit = SIZE_MAX;
        if (owner->byte_limit) {
#if SIZE_MAX < UINT64_MAX
            if (owner->byte_limit > SIZE_MAX) return XRT_MANAGED_LIMIT;
#endif
            limit = (size_t)owner->byte_limit;
        }
        XrtManagedStatus status = xrt_billing_owner_new(NULL, limit, &billing);
        if (status != XRT_MANAGED_OK) return status;
        void *memory = NULL;
        status = xrt_managed_metadata_allocate(billing, sizeof(struct XrtNativeValues), &memory);
        if (status != XRT_MANAGED_OK) {
            xrt_billing_owner_drop(billing);
            return status;
        }
        memset(memory, 0, sizeof(struct XrtNativeValues));
        struct XrtNativeValues *values = memory;
        values->billing = billing;
        owner->managed = values;
    }
    *output = owner->managed->billing;
    return XRT_MANAGED_OK;
}

XR_FUNC bool xrt_native_values_prepared(const XrtValueRoot *owner) {
    return owner && xrt_value_root_storage_empty(owner) &&
        (!owner->managed || (!owner->managed->root && !owner->managed->backings.string));
}
XR_FUNC XrtManagedStatus xrt_native_values_backings(XrtValueRoot *owner, const XrtBackingTypes **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(owner,&billing);
    if (status == XRT_MANAGED_OK && !owner->managed->backings.string)
        status = xrt_backing_types_init(billing,&owner->managed->backings);
    if (status == XRT_MANAGED_OK) *output = &owner->managed->backings;
    return status;
}

static struct XrtNativeValues *native_values(uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(owner, &billing);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    struct XrtNativeValues *values = owner->managed;
    if (!values->root) {
        status = xrt_managed_root_new(billing, &values->root);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    }
    return values;
}

XRAY_API XrtManagedRoot *xrt_native_managed_root(uint32_t source_id) {
    return native_values(source_id)->root;
}

XRAY_API void xrt_native_string_create(XrtManagedFrame *frame, size_t slot,
    const char *bytes, size_t length, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_create(frame, slot, types, bytes, length, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_concat_create(frame, slot, types,
            (const XrtStringBacking *)left, (const XrtStringBacking *)right, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_array_create(XrtManagedFrame *frame, size_t slot,
    const XrtArrayInput *input, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = input ? xrt_native_values_backings(xrt_value_root_current(source_id),&types) : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) {
        switch (input->kind) {
        case XRT_BACKING_ARRAY_I64: status=xrt_array_backing_i64_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_STRING: status=xrt_array_backing_string_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_U8: status=xrt_array_backing_u8_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_U64: status=xrt_array_backing_u64_create(frame,slot,types,input->values,input->count,input->count); break;
        default: status=XRT_MANAGED_ARGUMENT; break;
        }
    }
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_repeat(XrtManagedFrame *frame, size_t slot,
    const XrtArrayItem *item, int64_t length, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_backing_repeat_create(frame,slot,types,item,length);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_get(const XrtManagedHeader *array, XrtBackingArrayKind kind,
    int64_t index, XrtArrayItem *output, uint32_t source_id) {
    XrtManagedStatus status=xrt_array_backing_get((const XrtArrayBacking *)array,kind,index,output);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_display(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *array, XrtBackingArrayKind kind, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_backing_display_create(frame,slot,types,(const XrtArrayBacking *)array,kind);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_update_place(XrtPlace binding, XrtBackingArrayKind kind,
    XrtManagedFrame *frame, size_t workspace, const XrtArrayUpdate *update, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_update_place(binding,kind,frame,workspace,types,update);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
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
