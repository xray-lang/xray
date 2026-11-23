/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_file_publish.c - Stable owner and presence publication
 */
#include "xnative_host_file.h"
#include "xnative_host_io.h"
#include "xnative_managed_root.h"
#include "xnative_values.h"
#include "base/xchecks.h"
#include "xray_native_place.h"

typedef struct HostFilePublication { uint8_t *tag; uint8_t present; } HostFilePublication;
typedef struct HostFilePlaces {
    XrtPlace destination, temporary;
    const XrtManagedHeader *current;
    uint8_t *tag;
} HostFilePlaces;

static void host_file_publish_tag(void *context) {
    const HostFilePublication *publication = context;
    *publication->tag = publication->present;
}

static XrtManagedStatus host_file_places(const XrtNativeFileResult *output, XrtManagedRoot *root,
    size_t temporary_owner_offset, HostFilePlaces *places) {
    if (!output || output->destination == output->temporary || (root && xrt_managed_frame_root(output->frame) != root))
        return XRT_MANAGED_ARGUMENT;
    void *payload = NULL;
    size_t extent = 0;
    XrtManagedSlotLocation location;
    XrtManagedStatus status = xrt_managed_frame_payload(output->frame, &payload, &extent);
    if (status != XRT_MANAGED_OK || output->offset > extent || sizeof(XrtHostFileOptional) > extent - output->offset)
        return XRT_MANAGED_ARGUMENT;
    status = xrt_managed_slot_location(output->frame, output->destination, &location);
    if (status != XRT_MANAGED_OK || (uint8_t *)location.owner != (uint8_t *)payload + output->offset +
        offsetof(XrtHostFileOptional, owner)) return XRT_MANAGED_ARGUMENT;
    if (root) {
        XrtManagedSlotLocation temporary;
        status = xrt_managed_slot_location(output->frame, output->temporary, &temporary);
        if (status != XRT_MANAGED_OK || temporary.owner == location.owner ||
            temporary_owner_offset > extent || sizeof(*temporary.owner) > extent - temporary_owner_offset ||
            (uint8_t *)temporary.owner != (uint8_t *)payload + temporary_owner_offset)
            return XRT_MANAGED_ARGUMENT;
    }
    uint8_t *tag = xrt_managed_frame_scalar_byte(output->frame, output->offset);
    if (!tag) return XRT_MANAGED_ARGUMENT;
    HostFilePlaces resolved = {
        .destination={.frame=output->frame,.slot=output->destination,.nulls=XRT_PLACE_NULLABLE},
        .temporary={.frame=output->frame,.slot=output->temporary,.nulls=XRT_PLACE_NULLABLE},
        .tag=tag,
    };
    const XrtManagedHeader *staged = NULL;
    status = xrt_place_borrow(resolved.destination, &resolved.current);
    if (status == XRT_MANAGED_OK) status = xrt_place_borrow(resolved.temporary, &staged);
    if (status != XRT_MANAGED_OK) return status;
    if (staged || (root && *tag != (resolved.current != NULL))) return XRT_MANAGED_ARGUMENT;
    *places = resolved;
    return XRT_MANAGED_OK;
}

static XrtManagedStatus host_file_publish(const HostFilePlaces *places, bool present) {
    HostFilePublication presence = {places->tag, present};
    XrtPlacePublication publication = {host_file_publish_tag, &presence};
    return xrt_place_move_replace(places->destination, places->temporary, &publication);
}

XR_FUNC void xrt_host_file_open_publish(const XrtNativeFileResult *output, XrtBoundClass binding,
    const XrtStringBacking *text, bool append, uint32_t source_id) {
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(xrt_value_root_current(source_id), &billing);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    HostFilePlaces places;
    status = host_file_places(output, NULL, 0, &places);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);

    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrtHostFileOpen result = xrt_host_file_open_write(output->frame, output->temporary, binding, &policy, text, append);
    if (result.managed != XRT_MANAGED_OK) xrt_native_managed_failure(result.managed, source_id);
    if (result.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(result.host), source_id);
    status = host_file_publish(&places, result.opened);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XR_FUNC XrtHostFileOpen xrt_host_file_open_read_publish(const XrtHostFileStagedResult *output,
    XrtBoundClass binding, const XrtStringBacking *text, uint32_t source_id) {
    if (!output) return (XrtHostFileOpen){.managed=XRT_MANAGED_ARGUMENT};
    HostFilePlaces places;
    XrtManagedStatus status = host_file_places(&output->location, xrt_native_managed_root(source_id),
        output->temporary_owner_offset, &places);
    if (status != XRT_MANAGED_OK) return (XrtHostFileOpen){.managed=status};
    if (places.current && !xrt_class_backing_matches(places.current, binding.plan))
        return (XrtHostFileOpen){.managed=XRT_MANAGED_ARGUMENT};
    XrtBillingOwner *billing = NULL;
    status = xrt_native_values_prepare(xrt_value_root_current(source_id), &billing);
    if (status != XRT_MANAGED_OK) return (XrtHostFileOpen){.managed=status};
    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrtHostFileOpen result = xrt_host_file_open_read(places.temporary.frame, places.temporary.slot, binding, &policy, text);
    if (result.managed == XRT_MANAGED_OK && result.host == XR_OS_IO_OK)
        XR_CHECK(host_file_publish(&places, result.opened) == XRT_MANAGED_OK,
            "synchronous file open keeps its preflighted nullable home");
    return result;
}

XR_FUNC XrtHostFileRead xrt_host_file_read_publish(const XrtHostFileStagedResult *output,
    XrtBoundClass binding, const XrtManagedHeader *file, const XrtBackingTypes *types, int64_t max_bytes,
    uint32_t source_id) {
    if (!output) return (XrtHostFileRead){.managed=XRT_MANAGED_ARGUMENT};
    HostFilePlaces places;
    XrtManagedStatus status = host_file_places(&output->location, xrt_native_managed_root(source_id),
        output->temporary_owner_offset, &places);
    if (status != XRT_MANAGED_OK) return (XrtHostFileRead){.managed=status};
    if (places.current && !xrt_backing_array_matches(places.current, XRT_BACKING_ARRAY_U8))
        return (XrtHostFileRead){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostFileRead result = xrt_host_file_read(places.temporary.frame, places.temporary.slot, binding, file, types, max_bytes);
    if (result.managed == XRT_MANAGED_OK && result.host == XR_OS_IO_OK)
        XR_CHECK(host_file_publish(&places, result.present) == XRT_MANAGED_OK,
            "synchronous file read keeps its preflighted nullable home");
    return result;
}
