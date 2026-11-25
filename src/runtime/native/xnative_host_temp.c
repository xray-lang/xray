/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_temp.c - Prepare a managed result before exclusive creation
 */
#include "xnative_host_temp.h"
#include "xnative_execution.h"
#include "xnative_host_io.h"
#include "xnative_place.h"
#include "xnative_string_backing.h"
#include "xnative_values.h"

struct XrtHostTempRetry {
    XrtBillingOwner *billing;
    XrOsFileTempOwner owner;
};
typedef struct TempPreparation {
    XrtPlace scratch;
    const XrtBackingTypes *types;
    bool (*current)(void *);
    void *context;
    XrtManagedStatus status;
} TempPreparation;

static bool temp_current(void *context) {
    const TempPreparation *preparation = context;
    return !preparation->current || preparation->current(preparation->context);
}

static XrOsIoStatus temp_prepare(void *context, const char *text, size_t length) {
    TempPreparation *p = context;
    p->status = xrt_managed_slot_drop(p->scratch.frame, p->scratch.slot);
    if (p->status == XRT_MANAGED_OK)
        p->status = xrt_string_backing_create(p->scratch.frame, p->scratch.slot, p->types, text, length, 0);
    switch (p->status) {
    case XRT_MANAGED_OK: return XR_OS_IO_OK;
    case XRT_MANAGED_LIMIT: return XR_OS_IO_MEMORY_LIMIT;
    case XRT_MANAGED_OOM: return XR_OS_IO_OUT_OF_MEMORY;
    case XRT_MANAGED_OVERFLOW: return XR_OS_IO_OVERFLOW;
    default: return XR_OS_IO_BAD_ARGUMENT;
    }
}

static void temp_retry_free(XrtHostTempRetry *retry) {
    XrtBillingOwner *billing = retry->billing;
    xrt_managed_metadata_free(billing, retry, sizeof(*retry));
    xrt_billing_owner_drop(billing);
}

XR_FUNC XrOsIoStatus xrt_host_temp_retry(XrtHostTempRetry **retained) {
    if (!retained) return XR_OS_IO_BAD_ARGUMENT;
    XrtHostTempRetry *retry = *retained;
    if (!retry) return XR_OS_IO_OK;
    const XrOsIoPolicy policy = xrt_host_io_policy(retry->billing);
    XrOsIoStatus status = xr_os_io_temp_finish(&policy, &retry->owner, false);
    if (status == XR_OS_IO_OK) {
        *retained = NULL;
        temp_retry_free(retry);
    }
    return status;
}

static void temp_publish(const XrtHostNullablePlaces *places, XrtHostTempRetry *retry,
    const XrOsIoPolicy *policy, bool created, XrtHostTempResult *result) {
    XrtPlaceStagedMove move = {0};
    result->managed = xrt_place_staged_move_begin(places->destination.frame,
        places->destination.slot, places->temporary.slot, &move);
    if (result->managed != XRT_MANAGED_OK) return;

    const XrtManagedHeader *previous = *move.destination.owner, *candidate = *move.source.owner;
    if (previous != places->current || *places->tag != (previous != NULL) ||
        (created ? !xrt_backing_string_matches(candidate) : candidate != NULL))
        result->managed = XRT_MANAGED_ARGUMENT;
    else if (created) {
        /* The execution constructs the default billing allocator. Kernel keep
         * only closes its own handles and frees raw billed metadata. Neither
         * operation calls back into a Frame or managed value. */
        result->host = xr_os_io_temp_finish(policy, &retry->owner, true);
    }
    bool publish = result->managed == XRT_MANAGED_OK && result->host == XR_OS_IO_OK;
    XrtManagedHeader *retired = xrt_place_staged_move_end(&move, publish ? places->tag : NULL, created);
    result->present = publish && created;
    xrt_managed_drop(retired);
}

XR_FUNC XrtHostTempResult xrt_host_temp_publish(const XrtHostNullableSlots *output,
    const XrtManagedHeader *root, bool directory, bool (*current)(void *), void *context,
    uint32_t source_id, XrtHostTempRetry **retained) {
    if (!retained || *retained || !output || !xrt_backing_string_matches(root))
        return (XrtHostTempResult){.managed=XRT_MANAGED_ARGUMENT};
    if (!xr_os_io_temp_available()) return (XrtHostTempResult){.host=XR_OS_IO_UNSUPPORTED};
    if (current && !current(context)) return (XrtHostTempResult){.host=XR_OS_IO_CANCELLED};

    XrtValueRoot *execution = xrt_value_root_current(source_id);
    XrtManagedRoot *frame_root = xrt_native_values_existing_root(execution);
    if (!frame_root) return (XrtHostTempResult){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostNullablePlaces places;
    XrtHostTempResult result = {.managed=xrt_host_nullable_prepare(output, frame_root, &places)};
    if (result.managed != XRT_MANAGED_OK) return result;
    if (places.current && !xrt_backing_string_matches(places.current))
        return (XrtHostTempResult){.managed=XRT_MANAGED_ARGUMENT};

    XrtBillingOwner *billing = NULL;
    TempPreparation preparation = {.scratch=places.temporary,.current=current,.context=context};
    result.managed = xrt_native_values_prepare(execution, &billing);
    if (result.managed == XRT_MANAGED_OK)
        result.managed = xrt_native_values_backings(execution, &preparation.types);
    if (result.managed != XRT_MANAGED_OK) return result;
    if (!xrt_billing_owner_retain(billing)) return (XrtHostTempResult){.managed=XRT_MANAGED_REFCOUNT};
    void *memory = NULL;
    result.managed = xrt_managed_metadata_allocate(billing, sizeof(XrtHostTempRetry), &memory);
    if (result.managed != XRT_MANAGED_OK) {
        xrt_billing_owner_drop(billing);
        return result;
    }
    XrtHostTempRetry *retry = memory;
    *retry = (XrtHostTempRetry){.billing=billing};
    const XrtStringBacking *path = (const XrtStringBacking *)root;
    const XrOsIoPolicy policy = xrt_host_io_policy(billing);
    const XrOsFileTempRequest request = {&policy,{path->bytes,path->length},&preparation,temp_current,temp_prepare};
    bool created = false;
    result.host = directory ? xr_os_io_temp_directory(&request, &retry->owner, &created) :
        xr_os_io_temp_file(&request, &retry->owner, &created);
    result.managed = preparation.status;
    if (result.host == XR_OS_IO_OK && !temp_current(&preparation)) result.host = XR_OS_IO_CANCELLED;
    if (result.host == XR_OS_IO_OK && !created)
        result.managed = xrt_managed_slot_drop(output->frame, output->temporary);
    if (result.host == XR_OS_IO_OK && result.managed == XRT_MANAGED_OK)
        temp_publish(&places, retry, &policy, created, &result);
    if (result.host != XR_OS_IO_OK || result.managed != XRT_MANAGED_OK) {
        XrtManagedStatus cleared = xrt_managed_slot_drop(output->frame, output->temporary);
        if (result.managed == XRT_MANAGED_OK) result.managed = cleared;
    }
    if (retry->owner.state) {
        result.rollback = xrt_host_temp_retry(&retry);
        *retained = retry;
    } else temp_retry_free(retry);
    return result;
}

XR_FUNC XrtHostTempResult xrt_execution_temp_result(const XrtHostNullableSlots *output,
    const XrtManagedHeader *root, bool directory, bool (*current)(void *), void *context,
    uint32_t source_id) {
    XrtHostTempResult result = xrt_execution_receiving_admission(source_id);
    if (result.host != XR_OS_IO_OK || result.managed != XRT_MANAGED_OK) return result;
    XrtTempFailure failure = {.source_id=source_id};
    failure.result = xrt_host_temp_publish(output, root, directory, current, context, source_id,
        &failure.owner);
    if (failure.owner) xrt_execution_temp_failure_take(&failure);
    return failure.result;
}
