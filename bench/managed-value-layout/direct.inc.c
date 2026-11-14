/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * direct.inc.c - Scalar local access with descriptors only on cold writes
 *
 * KEY CONCEPT:
 *   The caller moves one real Array reference into this alias-free operation.
 *   The returned owner, including after failure, carries that same obligation.
 *   All generated RHS values here are plain integers and cannot observe owners.
 */
static XrMvlArrayWrite mvl_array_push_direct_range(XrMvlArray *owner, int64_t count) {
    XrMvlStatus status = XR_MVL_OK;
    int64_t *data = NULL;
    size_t length = 0, capacity = 0;
    if (count > 0) {
        int64_t value = 0;
        XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(owner), 0, value, true);
        if (write.status != XR_MVL_OK)
            return (XrMvlArrayWrite) {write.status, xr_mvl_array_access_start(owner), NULL};
        owner = write.access.owner;
        data = write.access.data;
        length = write.access.length;
        capacity = write.access.capacity;
        if (write.previous) xr_mvl_drop(&write.previous->header);
    }
    for (int64_t i = 1; i < count; ++i) {
        int64_t value = i;
        if (length < capacity) {
            data[length++] = value;
            owner->length = length;
        } else {
            XrMvlArrayAccess access = {owner, data, length, capacity, true};
            XrMvlArrayWrite write = xr_mvl_array_access_write(access, 0, value, true);
            if (write.status != XR_MVL_OK) {
                status = write.status;
                break;
            }
            owner = write.access.owner;
            data = write.access.data;
            length = write.access.length;
            capacity = write.access.capacity;
            if (write.previous) xr_mvl_drop(&write.previous->header);
        }
    }
    return (XrMvlArrayWrite) {status, xr_mvl_array_access_start(owner), NULL};
}
