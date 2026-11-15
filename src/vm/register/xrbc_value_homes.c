/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_homes.c - Resolve raw descriptors into actual stable Root slots
 */
#include "xrbc_value_homes.h"
XR_FUNCDEF XrtManagedStatus xr_rbc_value_homes_enter(const XrLirValueHomes *homes,
    XrtManagedRoot *root, XrtManagedFrame **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    if (!homes || (!homes->frame.slot_count && !homes->frame.payload_size)) return XRT_MANAGED_OK;
    return xrt_managed_frame_enter_layout(root, &homes->frame, output);
}
static const XrLirValueHome *owned_home(const XrLirValueHomes *homes, uint32_t value,
    bool temporary, size_t *offset, size_t *first) {
    if (!homes || !value || value > homes->value_count) return NULL;
    const XrLirValueHome *home = &homes->values[value];
    if (home->type == 0 || home->parameter) return NULL;
    *offset = temporary ? home->temporary_offset : home->offset;
    *first = temporary ? home->temporary_first_owner : home->first_owner;
    return *offset == SIZE_MAX ? NULL : home;
}
XR_FUNCDEF XrtManagedStatus xr_rbc_value_home_address(XrRbcHomeLocation location, void **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    size_t offset, first;
    const XrLirValueHomes *homes = location.homes;
    const XrLirValueHome *home = owned_home(homes, location.value, location.temporary, &offset, &first);
    if (!home) return XRT_MANAGED_ARGUMENT;
    void *payload = NULL; size_t bytes = 0;
    XrtManagedStatus status = xrt_managed_frame_payload(location.frame, &payload, &bytes);
    if (status != XRT_MANAGED_OK) return status;
    if (bytes != homes->frame.payload_size || offset > bytes || home->size > bytes - offset)
        return XRT_MANAGED_ARGUMENT;
    *output = (unsigned char *)payload + offset;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xr_rbc_value_home_place(XrRbcHomeLocation request, size_t leaf,
    XrtPlaceNull nulls, XrtPlace *output) {
    if (!output || (nulls != XRT_PLACE_NONNULL && nulls != XRT_PLACE_NULLABLE)) return XRT_MANAGED_ARGUMENT;
    size_t offset, first;
    const XrLirValueHomes *homes = request.homes;
    const XrLirValueHome *home = owned_home(homes, request.value, request.temporary, &offset, &first);
    if (!home || leaf >= home->owner_count || first > homes->frame.slot_count ||
        leaf >= homes->frame.slot_count - first) return XRT_MANAGED_ARGUMENT;
    void *address = NULL;
    XrtManagedStatus status = xr_rbc_value_home_address(request, &address);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedSlotLocation location;
    status = xrt_managed_slot_location(request.frame, first + leaf, &location);
    if (status != XRT_MANAGED_OK) return status;
    size_t leaf_offset = homes->frame.owned_offsets[first + leaf];
    if (leaf_offset < offset || leaf_offset - offset >= home->size ||
        (unsigned char *)location.owner != (unsigned char *)address + leaf_offset - offset)
        return XRT_MANAGED_ARGUMENT;
    *output = (XrtPlace){.frame=request.frame, .slot=first + leaf, .nulls=nulls};
    return XRT_MANAGED_OK;
}
