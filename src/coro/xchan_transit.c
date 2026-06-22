/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchan_transit.c - Channel transit reference cleanup.
 */

#include "xdeep_copy.h"
#include "../runtime/xshared.h"

void xr_chan_transit_release(XrValue value) {
    if (!XR_IS_PTR(value))
        return;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj || !XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT))
        return;
    /* Drop the channel-buffer reference. The graph's interior nodes hold
     * one reference each from their parent container, so destroying the
     * root cascades through the regular shared-destroy path. */
    if (xr_obj_drop_is_last(obj))
        xr_shared_destroy(obj);
}
