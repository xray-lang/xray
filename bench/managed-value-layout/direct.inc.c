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
static XrMvlArrayWrite mvl_array_push_direct_range(XrMvlArray array, int64_t count) {
    XrMvlStatus status = XR_MVL_OK;
    int64_t *data = NULL;
    size_t capacity = 0;
    if (count > 0) {
        int64_t value = 0;
        XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, value, true);
        if (write.status != XR_MVL_OK)
            return write;
        array = write.access.value;
        data = write.access.data;
        capacity = write.access.capacity;
        if (write.previous) xr_mvl_drop(&write.previous->header);
    }
    int64_t next = 1;
    while (next < count) {
        uint64_t remaining = (uint64_t)(count - next);
        size_t available = capacity - array.length;
        size_t append_count = remaining < available ? (size_t)remaining : available;
        for (size_t offset = 0; offset < append_count; ++offset)
            data[array.length + offset] = next + (int64_t)offset;
        next += (int64_t)append_count;
        // The segment has no calls. Publish the prefix before growth can fail.
        array.length += append_count;
        if (next == count)
            break;
        XrMvlArrayAccess access = {array, data, capacity, true};
        XrMvlArrayWrite write = xr_mvl_array_access_write(access, 0, next, true);
        if (write.status != XR_MVL_OK) {
            status = write.status;
            break;
        }
        array = write.access.value;
        data = write.access.data;
        capacity = write.access.capacity;
        if (write.previous) xr_mvl_drop(&write.previous->header);
        ++next;
    }
    return (XrMvlArrayWrite) {status, xr_mvl_array_access_start(array), NULL};
}
