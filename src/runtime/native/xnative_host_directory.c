/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_directory.c - Directory tokens owned by one execution root
 */
#include "xnative_host_directory.h"
#include "xnative_execution.h"
#include "xnative_host_io.h"
#include "xnative_values.h"
#include "base/xchecks.h"
#include <stdatomic.h>

struct XrtHostDirectory {
    XrtHostDirectory *next;
    XrOsDirectoryOwner owner;
    int64_t token;
    bool busy;
};

static XrtHostDirectory *directory_find(XrtHostDirectory *entry, int64_t token) {
    while (entry && entry->token != token) entry = entry->next;
    return entry && !entry->busy ? entry : NULL;
}

static void directory_dispose(XrtHostDirectory **first, XrtHostDirectory *entry,
    const XrOsIoPolicy *policy) {
    XR_CHECK(entry->busy && !entry->owner.state, "only a consumed directory can leave its root");
    XrtHostDirectory **link = first;
    while (*link != entry) {
        XR_CHECK(*link, "a busy directory remains registered in its root");
        link = &(*link)->next;
    }
    *link = entry->next;
    policy->free(policy->context, entry);
}

typedef struct DirectoryClose {
    XrOsIoStatus status;
    bool consumed;
} DirectoryClose;

static DirectoryClose directory_close_owned(XrtHostDirectory **first, XrtHostDirectory *entry,
    const XrOsIoPolicy *policy) {
    XR_CHECK(entry->busy, "a directory close owns the cursor until the provider returns");
    void *state = entry->owner.state;
    XrOsIoStatus status = xr_os_io_directory_close(&entry->owner);
    bool consumed = entry->owner.state == NULL;
    XR_CHECK(consumed || (status != XR_OS_IO_OK && entry->owner.state == state),
        "a retained cursor requires a close error and its original owner");
    if (consumed) directory_dispose(first, entry, policy);
    else entry->busy = false;
    return (DirectoryClose){status, consumed};
}

#if defined(XR_OS_WINDOWS) || defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
/* The counter owns no handles. Uniqueness prevents stale tokens from naming
 * a cursor in a later generation, even when the root storage is reused. */
static atomic_uint_fast64_t directory_sequence;

static int64_t directory_reserve_token(void) {
    uint_fast64_t sequence = atomic_load_explicit(&directory_sequence, memory_order_relaxed);
    while (sequence < INT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(&directory_sequence, &sequence, sequence + 1,
                memory_order_relaxed, memory_order_relaxed)) return (int64_t)(sequence + 1);
    }
    return 0;
}

static XrOsIoStatus directory_billing_status(XrtManagedStatus status) {
    switch (status) {
    case XRT_MANAGED_LIMIT: return XR_OS_IO_MEMORY_LIMIT;
    case XRT_MANAGED_OOM: return XR_OS_IO_OUT_OF_MEMORY;
    case XRT_MANAGED_OVERFLOW: return XR_OS_IO_OVERFLOW;
    default: return XR_OS_IO_BAD_ARGUMENT;
    }
}
#endif

XR_FUNC XrOsIoStatus xrt_host_directory_open(XrOsPathSpan path, int64_t *token) {
    XrtValueRoot *root = xrt_value_root_active();
    if (!root || root->implicit || !path.text || !token) return XR_OS_IO_BAD_ARGUMENT;
#if defined(XR_OS_WINDOWS) || defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
    const XrtHostExecutionControl *control = xrt_execution_host_control();
    if (control && !control->current(control->context)) return XR_OS_IO_CANCELLED;
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus prepared = xrt_native_values_prepare(root, &billing);
    if (prepared != XRT_MANAGED_OK) return directory_billing_status(prepared);
    XrtHostDirectory **first = xrt_native_values_directories(root, NULL);
    XR_CHECK(first, "a prepared explicit root supplies its own cursor list");
    int64_t reserved = directory_reserve_token();
    if (!reserved) return XR_OS_IO_OVERFLOW;

    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    void *memory = NULL;
    XrOsIoStatus status = policy.alloc(policy.context, sizeof(XrtHostDirectory), &memory);
    if (status != XR_OS_IO_OK) return status;
    XrtHostDirectory *entry = memory;
    *entry = (XrtHostDirectory){.next=*first, .busy=true};
    *first = entry;

    /* A failed open may transfer a real, unpublished owner to retirement. */
    XrOsDirectoryRequest request = {.policy=&policy, .path=path,
        .context=control ? control->context : NULL, .current=control ? control->current : NULL};
    bool opened = false;
    status = xr_os_io_directory_open(&request, &entry->owner, &opened);
    XR_CHECK(status != XR_OS_IO_OK || opened == (entry->owner.state != NULL),
        "an ordinary open result agrees with its owned cursor");
    if (status == XR_OS_IO_OK && control && !control->current(control->context)) status = XR_OS_IO_CANCELLED;
    if (status == XR_OS_IO_OK && opened) {
        entry->token = reserved;
        entry->busy = false;
        *token = reserved;
        return XR_OS_IO_OK;
    }
    if (!entry->owner.state) directory_dispose(first, entry, &policy);
    else entry->busy = false;
    if (status == XR_OS_IO_OK) *token = 0;
    return status;
#else
    return XR_OS_IO_UNSUPPORTED;
#endif
}

static XrOsIoStatus directory_close(int64_t token, bool *closed, const XrtHostExecutionControl *control) {
    if (!closed || token <= 0) return XR_OS_IO_BAD_ARGUMENT;
    XrtBillingOwner *billing = NULL;
    XrtHostDirectory **first = xrt_native_values_directories(xrt_value_root_active(), &billing);
    if (!first) return XR_OS_IO_BAD_ARGUMENT;
    XrtHostDirectory *entry = directory_find(*first, token);
    if (!entry) return XR_OS_IO_BAD_ARGUMENT;

    /* Host callbacks may change other entries. Only the busy owner and the
     * root's list head stay live across those calls. */
    entry->busy = true;
    if (control && !control->current(control->context)) {
        entry->busy = false;
        return XR_OS_IO_CANCELLED;
    }
    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    DirectoryClose result = directory_close_owned(first, entry, &policy);
    if (control && !control->current(control->context)) return XR_OS_IO_CANCELLED;
    *closed = result.status == XR_OS_IO_OK;
    return XR_OS_IO_OK;
}

XR_FUNC XrOsIoStatus xrt_host_directory_close(int64_t token, bool *closed) {
    return directory_close(token, closed, xrt_execution_host_control());
}

XR_FUNC XrOsIoStatus xrt_host_directory_cleanup_close(int64_t token, bool *closed) {
    return directory_close(token, closed, NULL);
}

XR_FUNC XrOsIoStatus xrt_host_directory_next(int64_t token, XrtHostDirectoryName *output) {
    XrtValueRoot *root = xrt_value_root_active();
    if (!root || root->implicit || token <= 0 || !output || output->text.bytes ||
        output->text.length || output->billing) return XR_OS_IO_BAD_ARGUMENT;
#if defined(XR_OS_WINDOWS) || defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
    XrtBillingOwner *billing = NULL;
    XrtHostDirectory **first = xrt_native_values_directories(root, &billing);
    if (!first) return XR_OS_IO_BAD_ARGUMENT;
    XrtHostDirectory *entry = directory_find(*first, token);
    if (!entry) return XR_OS_IO_BAD_ARGUMENT;

    /* Establish the new name's allocator lifetime before consuming an entry.
     * The provider commits the cursor only after producing an owned name. */
    if (!xrt_billing_owner_retain(billing)) return XR_OS_IO_OVERFLOW;
    entry->busy = true;
    const XrtHostExecutionControl *control = xrt_execution_host_control();
    XrOsDirectoryText text = {0};
    XrOsIoStatus status = xr_os_io_directory_next(&entry->owner,
        control ? control->context : NULL, control ? control->current : NULL, &text);
    /* The provider checks names before publishing and caches actual EOF.
     * Observe request cancellation after the last system query as well. */
    if (status == XR_OS_IO_END && control && !control->current(control->context))
        status = XR_OS_IO_CANCELLED;
    entry->busy = false;
    if (status == XR_OS_IO_OK) *output = (XrtHostDirectoryName){.text=text, .billing=billing};
    else xrt_billing_owner_drop(billing);
    return status;
#else
    return XR_OS_IO_UNSUPPORTED;
#endif
}

XR_FUNC void xrt_host_directory_name_drop(XrtHostDirectoryName *name) {
    if (!name || !name->billing) return;
    XrtHostDirectoryName owned = *name;
    *name = (XrtHostDirectoryName){0};
    XrOsIoPolicy policy = xrt_host_io_policy(owned.billing);
    policy.free(policy.context, owned.text.bytes);
    xrt_billing_owner_drop(owned.billing);
}

XR_FUNC XrOsIoStatus xrt_host_directory_drain(XrtHostDirectory **first, XrtBillingOwner *billing) {
    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrOsIoStatus status = XR_OS_IO_OK;
    while (*first) {
        XrtHostDirectory *entry = *first;
        if (entry->busy) return status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : status;
        entry->busy = true;
        DirectoryClose result = directory_close_owned(first, entry, &policy);
        if (status == XR_OS_IO_OK) status = result.status;
        if (!result.consumed) break;
    }
    return status;
}
