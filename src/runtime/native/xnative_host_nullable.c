/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_nullable.c - Stable owner and presence publication for host leaves
 */
#include "xnative_host_nullable.h"
#include "xnative_managed_root.h"

typedef struct HostNullablePublication { uint8_t *tag; uint8_t present; } HostNullablePublication;
static void host_nullable_publish_tag(void *context) {
    const HostNullablePublication *publication = context;
    *publication->tag = publication->present;
}

XR_FUNC XrtManagedStatus xrt_host_nullable_prepare(const XrtHostNullableSlots *output, XrtManagedRoot *root,
    XrtHostNullablePlaces *places) {
    if (!output || !places || output->destination == output->temporary || (root && xrt_managed_frame_root(output->frame) != root))
        return XRT_MANAGED_ARGUMENT;
    void *payload = NULL;
    size_t extent = 0;
    XrtManagedSlotLocation location;
    XrtManagedStatus status = xrt_managed_frame_payload(output->frame, &payload, &extent);
    if (status != XRT_MANAGED_OK || output->offset > extent || sizeof(XrtHostNullableOwner) > extent - output->offset)
        return XRT_MANAGED_ARGUMENT;
    status = xrt_managed_slot_location(output->frame, output->destination, &location);
    if (status != XRT_MANAGED_OK || (uint8_t *)location.owner != (uint8_t *)payload + output->offset +
        offsetof(XrtHostNullableOwner, owner)) return XRT_MANAGED_ARGUMENT;
    if (root) {
        XrtManagedSlotLocation temporary;
        status = xrt_managed_slot_location(output->frame, output->temporary, &temporary);
        if (status != XRT_MANAGED_OK || temporary.owner == location.owner ||
            output->temporary_owner_offset > extent || sizeof(*temporary.owner) > extent - output->temporary_owner_offset ||
            (uint8_t *)temporary.owner != (uint8_t *)payload + output->temporary_owner_offset)
            return XRT_MANAGED_ARGUMENT;
    }
    uint8_t *tag = xrt_managed_frame_scalar_byte(output->frame, output->offset);
    if (!tag) return XRT_MANAGED_ARGUMENT;
    XrtHostNullablePlaces resolved = {
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

XR_FUNC XrtManagedStatus xrt_host_nullable_publish(const XrtHostNullablePlaces *places, bool present) {
    HostNullablePublication presence = {places->tag, present};
    XrtPlacePublication publication = {host_nullable_publish_tag, &presence};
    return xrt_place_move_replace(places->destination, places->temporary, &publication);
}
