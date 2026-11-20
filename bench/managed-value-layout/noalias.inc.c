/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * noalias.inc.c - Capacity-bounded C11 restrict windows on canonical storage
 *
 * KEY CONCEPT:
 *   Elements and canonical length occupy disjoint subobjects. Within each
 *   nested block they use only their respective restricted pointer. No call,
 *   growth, retirement or restricted pointer escapes the block.
 */
static XrMvlArrayWrite mvl_array_push_noalias_range(XrMvlArray array, int64_t count) {
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
        {
            size_t *restrict length = &array.length;
            int64_t *restrict block_data = data;
            while (next < count && *length < capacity) {
                int64_t value = next;
                block_data[*length] = value;
                ++*length;
                ++next;
            }
        }
        if (next == count)
            break;
        /* Only unrestricted scalar copies survive the preceding block. A cold
         * write can replace storage; its result starts the next fresh window. */
        int64_t value = next;
        XrMvlArrayAccess access = {array, data, capacity, true};
        XrMvlArrayWrite write = xr_mvl_array_access_write(access, 0, value, true);
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
