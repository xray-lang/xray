/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_links.c - Publish the text one path query names
 */
#include "xnative_host_links.h"
#include "xnative_execution.h"
#include "xnative_host_io.h"
#include "xnative_string_backing.h"
#include "xnative_values.h"
#include "base/xchecks.h"
#include "xray_native_values.h"

XR_FUNC XrtHostLinkResult xrt_host_link_text_publish(const XrtHostNullableSlots *output,
    const XrtManagedHeader *path, bool follow, uint32_t source_id) {
    if (!output || !xrt_backing_string_matches(path))
        return (XrtHostLinkResult){.managed=XRT_MANAGED_ARGUMENT};
    XrtValueRoot *execution = xrt_value_root_current(source_id);
    XrtManagedRoot *frame_root = xrt_native_values_existing_root(execution);
    if (!frame_root) return (XrtHostLinkResult){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostNullablePlaces places;
    XrtHostLinkResult result = {.managed=xrt_host_nullable_prepare(output, frame_root, &places)};
    if (result.managed != XRT_MANAGED_OK) return result;
    if (places.current && !xrt_backing_string_matches(places.current))
        return (XrtHostLinkResult){.managed=XRT_MANAGED_ARGUMENT};

    XrtBillingOwner *billing = NULL;
    XrOsFileMetadataOwner *owner = NULL;
    result.managed = xrt_native_values_stat_prepare(execution, &billing, &owner);
    const XrtBackingTypes *types = NULL;
    if (result.managed == XRT_MANAGED_OK) result.managed = xrt_native_values_backings(execution, &types);
    if (result.managed != XRT_MANAGED_OK) return result;

    const XrOsIoPolicy policy = xrt_host_io_policy(billing);
    const XrtStringBacking *text = (const XrtStringBacking *)path;
    const XrOsPathSpan span = {text->bytes, text->length};
    XrOsFileLinkText answer = {0};
    result.host = follow ? xr_os_io_path_realpath(&policy, span, NULL, owner, &answer) :
        xr_os_io_path_readlink(&policy, span, NULL, owner, &answer);
    /* Every answer the host cannot give is this leaf's empty optional: a
     * missing path, a leaf that is not a link, a target outside the proven
     * native bound, or a refusal after the query began. */
    bool present = result.host == XR_OS_IO_OK && answer.text != NULL;
    if (present)
        result.managed = xrt_string_backing_create(places.temporary.frame, places.temporary.slot,
            types, answer.text, answer.length, 0);
    if (answer.text) policy.free(policy.context, answer.text);
    if (result.managed != XRT_MANAGED_OK || result.host != XR_OS_IO_OK) {
        XrtManagedStatus cleared = xrt_managed_slot_drop(output->frame, output->temporary);
        if (result.managed == XRT_MANAGED_OK) result.managed = cleared;
        return result;
    }
    /* A recoverable close failure is the query's own host status, which the
     * caller maps to the empty optional; only a still-open handle here would
     * mean the transport broke its contract. */
    XR_CHECK(!owner->handle, "a successful link query closes its handle before publishing");
    result.managed = xrt_host_nullable_publish(&places, present);
    result.present = present && result.managed == XRT_MANAGED_OK;
    return result;
}
