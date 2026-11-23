/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_environment.c - Bill, copy and release one host value
 */
#include "xnative_host_environment.h"
#include "xnative_host_io.h"
#include "xnative_string_backing.h"
#include "xnative_values.h"
#include "xray_native_values.h"
#include "base/xchecks.h"
#include "os/os_proc.h"
#include <string.h>

XR_FUNC XrtHostEnvironmentGet xrt_host_environment_get(XrtManagedFrame *frame, size_t slot,
    XrtBillingOwner *billing, const XrtBackingTypes *types, const XrtManagedHeader *name) {
    if (!billing || !types || !types->string || !xrt_backing_string_matches(name))
        return (XrtHostEnvironmentGet){.managed=XRT_MANAGED_ARGUMENT};
    /* getenv observes the terminated prefix even when a String contains an
     * embedded NUL. Keep that host lookup contract. */
    const char *key = ((const XrtStringBacking *)name)->bytes;
    if (!*key || strchr(key, '=')) return (XrtHostEnvironmentGet){0};

    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    char *scratch = NULL;
    XrtHostEnvironmentGet result = {.host=xr_os_io_environment_get(&policy, key, &scratch)};
    if (result.host != XR_OS_IO_OK) {
        if (result.host == XR_OS_IO_NOT_FOUND) result.host = XR_OS_IO_OK;
        return result;
    }
    XR_CHECK(scratch, "a successful host lookup owns its terminated UTF-8 result");
    result.managed = xrt_string_backing_create(frame, slot, types, scratch, strlen(scratch), 0);
    policy.free(policy.context, scratch);
    result.present = result.managed == XRT_MANAGED_OK;
    return result;
}

XR_FUNC XrtHostEnvironmentGet xrt_host_environment_get_publish(const XrtHostNullableSlots *output,
    const XrtManagedHeader *name, uint32_t source_id) {
    if (!xrt_backing_string_matches(name)) return (XrtHostEnvironmentGet){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostNullablePlaces places;
    XrtManagedStatus status = xrt_host_nullable_prepare(output, xrt_native_managed_root(source_id), &places);
    if (status != XRT_MANAGED_OK) return (XrtHostEnvironmentGet){.managed=status};
    if (places.current && !xrt_backing_string_matches(places.current))
        return (XrtHostEnvironmentGet){.managed=XRT_MANAGED_ARGUMENT};

    XrtValueRoot *execution = xrt_value_root_current(source_id);
    XrtBillingOwner *billing = NULL;
    const XrtBackingTypes *types = NULL;
    status = xrt_native_values_prepare(execution, &billing);
    if (status == XRT_MANAGED_OK) status = xrt_native_values_backings(execution, &types);
    if (status != XRT_MANAGED_OK) return (XrtHostEnvironmentGet){.managed=status};
    XrtHostEnvironmentGet result = xrt_host_environment_get(places.temporary.frame, places.temporary.slot,
        billing, types, name);
    if (result.managed == XRT_MANAGED_OK && result.host == XR_OS_IO_OK)
        XR_CHECK(xrt_host_nullable_publish(&places, result.present) == XRT_MANAGED_OK,
            "environment lookup keeps its preflighted nullable home");
    return result;
}
