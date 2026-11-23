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
#include "xray_native_place.h"

typedef struct HostFilePublication { uint8_t *tag; uint8_t present; } HostFilePublication;
static void host_file_publish_tag(void *context) {
    const HostFilePublication *publication = context;
    *publication->tag = publication->present;
}

XR_FUNC void xrt_host_file_open_publish(const XrtNativeFileResult *output, XrtBoundClass binding,
    const XrtStringBacking *text, bool append, uint32_t source_id) {
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(xrt_value_root_current(source_id), &billing);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    if (!output || output->destination == output->temporary)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    void *payload = NULL;
    size_t extent = 0;
    XrtManagedSlotLocation location;
    status = xrt_managed_frame_payload(output->frame, &payload, &extent);
    if (status != XRT_MANAGED_OK || output->offset > extent || sizeof(XrtHostFileOptional) > extent - output->offset)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    status = xrt_managed_slot_location(output->frame, output->destination, &location);
    if (status != XRT_MANAGED_OK || (uint8_t *)location.owner != (uint8_t *)payload + output->offset +
        offsetof(XrtHostFileOptional, owner)) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    uint8_t *tag = xrt_managed_frame_scalar_byte(output->frame, output->offset);
    if (!tag) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    XrtPlace destination = {.frame=output->frame,.slot=output->destination,.nulls=XRT_PLACE_NULLABLE};
    XrtPlace temporary = {.frame=output->frame,.slot=output->temporary,.nulls=XRT_PLACE_NULLABLE};
    const XrtManagedHeader *current = NULL;
    const XrtManagedHeader *staged = NULL;
    status = xrt_place_borrow(destination, &current);
    if (status == XRT_MANAGED_OK) status = xrt_place_borrow(temporary, &staged);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    if (staged) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);

    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrtHostFileOpen result = xrt_host_file_open_write(output->frame, output->temporary, binding, &policy, text, append);
    if (result.managed != XRT_MANAGED_OK) xrt_native_managed_failure(result.managed, source_id);
    if (result.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(result.host), source_id);
    HostFilePublication presence = {tag, result.opened};
    XrtPlacePublication publication = {host_file_publish_tag, &presence};
    status = xrt_place_move_replace(destination, temporary, &publication);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
